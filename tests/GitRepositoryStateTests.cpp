#include "TestSupport.h"

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

// A tracked file whose name begins with a space must keep that space: the parser
// previously trimmed leading spaces after the fixed token fields, so " leading.cpp"
// was reported as "leading.cpp" and stage/diff/discard would target the wrong path.
void TestPorcelainV2PathWithLeadingSpace() {
  // "1 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <path>"; the path here is " leading.cpp",
  // so there are two spaces after "def": the field delimiter plus the path's own
  // leading space.
  std::string output = "1 .M. N... 100644 100644 100644 abc def  leading.cpp";
  output.push_back('\0');
  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);
  bool saw_leading_space = false;
  for (const auto& entry : state.entries) {
    if (entry.path.relative_path == std::filesystem::path(" leading.cpp")) {
      saw_leading_space = true;
    }
  }
  Expect(saw_leading_space,
         "a path beginning with a space must be preserved verbatim, not trimmed");
}

// An UNTRACKED ('?') record's body is the whole path, which may legally begin with a
// space. The parser used to compute a leading XY field (and drop the record when it
// was empty) for every kind — so a "? \0 leading.txt" untracked entry with a
// leading-space name was silently discarded (no tree badge, never surfaced).
void TestPorcelainV2UntrackedPathWithLeadingSpace() {
  std::string output = "?  leading.txt";  // '?' + delimiter space + path " leading.txt"
  output.push_back('\0');
  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);
  bool saw_untracked_leading_space = false;
  for (const auto& entry : state.entries) {
    if (entry.kind == GitRepositoryEntryKind::Untracked &&
        entry.path.relative_path == std::filesystem::path(" leading.txt")) {
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
  Expect(state.entries.front().path.relative_path == std::filesystem::path("new-name.txt"),
         "rename fixture should preserve the new path");
  Expect(state.entries.front().old_path.has_value(), "rename fixture should include old path");
  Expect(state.entries.front().old_path->relative_path == std::filesystem::path("old.txt"),
         "rename fixture old path mismatch");
  // Regression: the rename SOURCE no longer exists at its old path, so the tree
  // status map badges it Deleted rather than inheriting the destination's status.
  Expect(state.tree_git_statuses.count("old.txt") == 1 &&
             state.tree_git_statuses.at("old.txt") == GitFileStatus::Deleted,
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
        entry.path.relative_path == std::filesystem::path("new name.txt")) {
      saw_rename = entry.old_path.has_value() &&
                    entry.old_path->relative_path == std::filesystem::path("old path.txt");
    }
  }
  Expect(saw_rename, "rename pair should preserve spaced paths");
}

void TestPorcelainV2RenameSourceStartingWithStatusSigil() {
  // Regression: the rename origPath is the next NUL field and must be consumed
  // unconditionally. A source filename whose first byte collides with a record
  // sigil ('#', '1', '2', 'u', '?', '!') was previously rejected by a first-byte
  // heuristic, dropping old_path and re-parsing the source as a bogus record.
  // Two renames whose sources start with '2' and 'u' exercise both.
  std::string output = "2 R. N... 100644 100644 100644 abc def R100 renamed_a.txt";
  output.push_back('\0');
  output.append("2data.txt");  // source begins with '2'
  output.push_back('\0');
  output.append("2 R. N... 100644 100644 100644 abc def R100 renamed_b.txt");
  output.push_back('\0');
  output.append("user.txt");  // source begins with 'u'
  output.push_back('\0');

  const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 5, 0);
  Expect(state.entries.size() == 2,
         "both renames should parse without the source being misread as an entry");

  bool saw_a = false;
  bool saw_b = false;
  for (const auto& entry : state.entries) {
    Expect(entry.kind == GitRepositoryEntryKind::Renamed, "both entries should be renames");
    if (entry.path.relative_path == std::filesystem::path("renamed_a.txt")) {
      saw_a = entry.old_path.has_value() &&
              entry.old_path->relative_path == std::filesystem::path("2data.txt");
    }
    if (entry.path.relative_path == std::filesystem::path("renamed_b.txt")) {
      saw_b = entry.old_path.has_value() &&
              entry.old_path->relative_path == std::filesystem::path("user.txt");
    }
  }
  Expect(saw_a, "rename source '2data.txt' should be preserved as old_path");
  Expect(saw_b, "rename source 'user.txt' should be preserved as old_path");
  // The old paths must also carry a tree git status badge.
  Expect(state.tree_git_statuses.count("2data.txt") == 1,
         "rename source should receive a tree git status");
  Expect(state.tree_git_statuses.count("user.txt") == 1,
         "rename source should receive a tree git status");
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

// porcelain-v2 `# branch.ab` reports ahead/behind as `+<ahead> -<behind>`.
// ParseAheadBehind must (a) accept genuine git output including its space field
// separator, and (b) reject a field carrying a trailing non-numeric suffix
// instead of silently truncating it (e.g. "+12x-3" previously parsed ahead=12,
// "+12-3x" parsed behind=3 because only from_chars' error code was checked).
void TestPorcelainV2AheadBehindParsing() {
  const auto parse_ab = [](std::string_view token) {
    std::string output = "# branch.ab ";
    output.append(token);
    output.push_back('\0');
    const auto state = GitPorcelainV2Parser::Parse(output, "/repo", 1, 0);
    return std::pair<int, int>{state.branch.ahead, state.branch.behind};
  };

  // No-space form parses both counts.
  Expect(parse_ab("+12-3") == (std::pair<int, int>{12, 3}),
         "\"+12-3\" should parse ahead=12 behind=3");
  // Real git emits a space field separator; it must still parse (regression guard
  // against the full-consume check over-rejecting authentic output).
  Expect(parse_ab("+2 -0") == (std::pair<int, int>{2, 0}),
         "genuine git \"+2 -0\" must parse ahead=2 behind=0");

  // A trailing garbage suffix on either field is rejected outright, leaving the
  // default 0/0 rather than a truncated count.
  Expect(parse_ab("+12x-3") == (std::pair<int, int>{0, 0}),
         "\"+12x-3\" must be rejected, not truncated to ahead=12");
  Expect(parse_ab("+12-3x") == (std::pair<int, int>{0, 0}),
         "\"+12-3x\" must be rejected, not truncated to behind=3");
  // Non-numeric fields are rejected too.
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

// `<gitdir>/MERGE_HEAD` is read directly (no `git` subprocess) so the merge
// resolver can label the incoming pane from the shell thread without risking a
// 60 s git stall. Covers the plain `.git` directory, the linked-worktree
// `.git`-file indirection, octopus merges (first id wins), and the rejection of
// anything that is not a plain object id.
void TestReadPendingMergeHeadId() {
  namespace gitutil = microide::project::internal;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");

  Expect(!gitutil::ReadPendingMergeHeadId(root).has_value(),
         "no MERGE_HEAD means no pending merge");

  const std::string oid = "0123456789abcdef0123456789abcdef01234567";
  WriteFile(root / ".git/MERGE_HEAD", oid + "\n");
  Expect(gitutil::ReadPendingMergeHeadId(root) == oid, "a plain MERGE_HEAD id is read verbatim");

  // Octopus merge: one id per line, first wins (matches `git rev-parse MERGE_HEAD`).
  const std::string second = "89abcdef0123456789abcdef0123456789abcdef";
  WriteFile(root / ".git/MERGE_HEAD", oid + "\n" + second + "\n");
  Expect(gitutil::ReadPendingMergeHeadId(root) == oid, "an octopus merge reports the first id");

  // CRLF and trailing blanks must not leak into the label.
  WriteFile(root / ".git/MERGE_HEAD", oid + "  \r\n");
  Expect(gitutil::ReadPendingMergeHeadId(root) == oid, "trailing whitespace/CR is trimmed");

  for (const std::string_view junk : {"ref: refs/heads/topic", "not-hex-at-all-xxxxx", "abc", ""}) {
    WriteFile(root / ".git/MERGE_HEAD", std::string(junk) + "\n");
    Expect(!gitutil::ReadPendingMergeHeadId(root).has_value(),
           "a MERGE_HEAD that is not a plain object id is rejected");
  }

  // Linked worktree: `.git` is a FILE pointing at the real git directory.
  const std::filesystem::path worktree = temp_dir.path() / "wt";
  const std::filesystem::path linked_gitdir = temp_dir.path() / "gitdir";
  std::filesystem::create_directories(worktree);
  std::filesystem::create_directories(linked_gitdir);
  WriteFile(worktree / ".git", "gitdir: " + linked_gitdir.string() + "\n");
  WriteFile(linked_gitdir / "MERGE_HEAD", oid + "\n");
  Expect(gitutil::ReadPendingMergeHeadId(worktree) == oid,
         "a `.git` file indirection resolves to the linked git directory");
  Expect(gitutil::ResolveGitDirectory(worktree) == linked_gitdir,
         "ResolveGitDirectory follows the gitdir: pointer");

  // A relative gitdir: pointer resolves against the worktree root.
  const std::filesystem::path relative_worktree = temp_dir.path() / "rel";
  std::filesystem::create_directories(relative_worktree / "store");
  WriteFile(relative_worktree / ".git", "gitdir: store\n");
  WriteFile(relative_worktree / "store/MERGE_HEAD", oid + "\n");
  Expect(gitutil::ReadPendingMergeHeadId(relative_worktree) == oid,
         "a relative gitdir: pointer resolves against the worktree root");

  WriteFile(relative_worktree / ".git", "not a gitdir pointer\n");
  Expect(!gitutil::ResolveGitDirectory(relative_worktree).has_value(),
         "a `.git` file without a gitdir: pointer yields no git directory");
}

// `operation_state` is derived from git's own in-flight markers. It was a
// declared-but-never-written field, so the merge resolver could never report a
// rebase/cherry-pick incoming side.
void TestDetectGitOperationState() {
  using microide::project::DetectGitOperationState;
  using microide::project::GitOperationStateKind;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");

  Expect(DetectGitOperationState(root) == GitOperationStateKind::None,
         "a clean repository reports no in-flight operation");
  Expect(DetectGitOperationState(temp_dir.path() / "missing") == GitOperationStateKind::None,
         "a path with no git directory reports no in-flight operation");

  WriteFile(root / ".git/BISECT_LOG", "log\n");
  Expect(DetectGitOperationState(root) == GitOperationStateKind::Bisect,
         "BISECT_LOG reports a bisect");
  WriteFile(root / ".git/REVERT_HEAD", "0123456789abcdef0123456789abcdef01234567\n");
  Expect(DetectGitOperationState(root) == GitOperationStateKind::Revert,
         "REVERT_HEAD outranks BISECT_LOG");
  WriteFile(root / ".git/CHERRY_PICK_HEAD", "0123456789abcdef0123456789abcdef01234567\n");
  Expect(DetectGitOperationState(root) == GitOperationStateKind::CherryPick,
         "CHERRY_PICK_HEAD outranks REVERT_HEAD");
  std::filesystem::create_directories(root / ".git/rebase-merge");
  Expect(DetectGitOperationState(root) == GitOperationStateKind::Rebase,
         "a rebase state directory outranks the sequencer heads");
  WriteFile(root / ".git/MERGE_HEAD", "0123456789abcdef0123456789abcdef01234567\n");
  Expect(DetectGitOperationState(root) == GitOperationStateKind::Merge,
         "MERGE_HEAD outranks every other marker, matching git's own precedence");

  // `rebase-apply` (the am-based rebase layout) counts too.
  const std::filesystem::path apply_root = temp_dir.path() / "apply";
  std::filesystem::create_directories(apply_root / ".git/rebase-apply");
  Expect(DetectGitOperationState(apply_root) == GitOperationStateKind::Rebase,
         "rebase-apply also reports a rebase");
}

}  // namespace

void RegisterGitRepositoryStateTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitRepositoryState/PorcelainV2WorkingTreeFixture",
          TestPorcelainV2WorkingTreeFixture);
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
  AddTest(tests, "GitRepositoryState/ReadPendingMergeHeadId", TestReadPendingMergeHeadId);
  AddTest(tests, "GitRepositoryState/DetectGitOperationState", TestDetectGitOperationState);
}

}  // namespace microide::tests
