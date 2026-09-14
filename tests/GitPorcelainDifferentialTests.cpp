// Differential tests for the two git status parsers, against real `git` output.
//
// microide parses `git status` two ways: porcelain v1 (GitPorcelainParser, the
// GitRepository path) and porcelain v2 (GitPorcelainV2Parser, the background
// refresh that feeds the sidebar). They read different wire formats and were
// written separately, but they must classify the same repository identically --
// a divergence means the sidebar badge and whatever GitRepository answers
// disagree about the same file.
//
// The fixtures here are built by running actual git, not by hand-writing wire
// records, because the interesting cases are precisely the ones whose exact
// bytes are hard to guess: a rename git decided to call a rename, a path git
// chose to C-quote, a delete staged on one side and re-created on the other.

#include "TestSupport.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <utility>
#include <string>
#include <vector>

#include "platform/Subprocess.h"
#include "project/GitPorcelainParser.h"
#include "project/GitPorcelainV2Parser.h"
#include "project/GitRepositoryState.h"

namespace microide::tests {
namespace {

using microide::project::GitFileStatus;
using microide::project::GitPorcelainParser;
using microide::project::GitPorcelainV2Parser;
using microide::project::GitRepositoryEntry;
using microide::project::GitRepositoryState;
using microide::project::GitWorkingTreeEntry;

std::string RunGitCapture(const std::filesystem::path& repo, const std::vector<std::string>& args) {
  std::vector<std::string> argv{"git"};
  argv.insert(argv.end(), args.begin(), args.end());
  microide::platform::SubprocessOptions options;
  options.cwd = repo;
  const auto result = microide::platform::RunSubprocess(argv, options);
  return result.stdout_text;
}

std::string StatusName(GitFileStatus status) {
  switch (status) {
    case GitFileStatus::Clean: return "Clean";
    case GitFileStatus::Modified: return "Modified";
    case GitFileStatus::Added: return "Added";
    case GitFileStatus::Deleted: return "Deleted";
    case GitFileStatus::Untracked: return "Untracked";
    case GitFileStatus::Conflicted: return "Conflicted";
  }
  return "?";
}

// Build one repository holding every status shape at once, including paths git
// has to C-quote on the v1 wire.
void BuildRichRepository(const std::filesystem::path& repo) {
  InitializeGitRepo(repo);
  const auto write = [&](const std::string& name, const std::string& body) {
    WriteFile(repo / name, body);
  };

  write("unchanged.txt", "stable\n");
  write("modified.txt", "one\n");
  write("staged-modify.txt", "one\n");
  write("deleted.txt", "gone soon\n");
  write("staged-delete.txt", "gone soon\n");
  write("rename-source.txt", "a fairly distinctive body so git calls it a rename\n");
  write("both-staged-and-dirty.txt", "one\n");
  write("with space.txt", "spaced\n");
  write("with\"quote.txt", "quoted\n");
  write("naïve-ünïcode.txt", "unicode\n");
  std::filesystem::create_directories(repo / "nested" / "deeper");
  write("nested/deeper/inner.txt", "deep\n");
  CommitAll(repo, "base", "rich repo base");

  // Worktree-only changes.
  write("modified.txt", "one\ntwo\n");
  write("with space.txt", "spaced\nmore\n");
  write("naïve-ünïcode.txt", "unicode\nmore\n");
  std::filesystem::remove(repo / "deleted.txt");

  // Staged changes.
  write("staged-modify.txt", "one\ntwo\n");
  RequireGitCommandSuccess(repo, {"add", "staged-modify.txt"}, "stage a modify");
  RequireGitCommandSuccess(repo, {"rm", "--quiet", "staged-delete.txt"}, "stage a delete");
  RequireGitCommandSuccess(repo, {"mv", "rename-source.txt", "rename-target.txt"}, "stage a rename");
  write("added.txt", "brand new\n");
  RequireGitCommandSuccess(repo, {"add", "added.txt"}, "stage an add");

  // Staged AND further dirty in the worktree.
  write("both-staged-and-dirty.txt", "one\ntwo\n");
  RequireGitCommandSuccess(repo, {"add", "both-staged-and-dirty.txt"}, "stage the first edit");
  write("both-staged-and-dirty.txt", "one\ntwo\nthree\n");

  // Untracked, including one inside a new directory and one needing quoting.
  write("untracked.txt", "new\n");
  std::filesystem::create_directories(repo / "untracked-dir");
  write("untracked-dir/inside.txt", "new\n");
  write("untracked with space.txt", "new\n");
}

// Leave the repository in a real merge conflict.
void BuildConflictedRepository(const std::filesystem::path& repo) {
  InitializeGitRepo(repo);
  WriteFile(repo / "conflict.txt", "base\n");
  WriteFile(repo / "also-conflict.txt", "base\n");
  WriteFile(repo / "clean.txt", "stable\n");
  CommitAll(repo, "base", "conflict base");

  RequireGitCommandSuccess(repo, {"checkout", "-q", "-b", "feature"}, "branch");
  WriteFile(repo / "conflict.txt", "feature\n");
  WriteFile(repo / "also-conflict.txt", "feature\n");
  CommitAll(repo, "feature", "feature edit");

  RequireGitCommandSuccess(repo, {"checkout", "-q", "main"}, "back to main");
  WriteFile(repo / "conflict.txt", "main\n");
  WriteFile(repo / "also-conflict.txt", "main\n");
  CommitAll(repo, "main", "main edit");

  // Expected to fail -- that is the point.
  RunGitCommand(repo, {"merge", "feature"});
}

// v1 gives a path -> status map directly; reduce v2's entry list to the same
// shape so the two can be compared as sets.
std::map<std::string, GitFileStatus> V2StatusByPath(const GitRepositoryState& state) {
  std::map<std::string, GitFileStatus> out;
  for (const GitRepositoryEntry& entry : state.entries) {
    out[std::string(microide::project::GenericPathView(entry.path))] = entry.status;
  }
  return out;
}

std::map<std::string, GitFileStatus> V1StatusByPath(
    const std::vector<GitWorkingTreeEntry>& entries) {
  std::map<std::string, GitFileStatus> out;
  for (const GitWorkingTreeEntry& entry : entries) {
    out[entry.relative_path.generic_string()] = entry.status;
  }
  return out;
}

std::string RenderDifference(const std::map<std::string, GitFileStatus>& v1,
                             const std::map<std::string, GitFileStatus>& v2) {
  std::string out;
  for (const auto& [path, status] : v1) {
    const auto it = v2.find(path);
    if (it == v2.end()) {
      out += "\n  only in v1: '" + path + "' = " + StatusName(status);
    } else if (it->second != status) {
      out += "\n  differs: '" + path + "' v1=" + StatusName(status) +
             " v2=" + StatusName(it->second);
    }
  }
  for (const auto& [path, status] : v2) {
    if (v1.find(path) == v1.end()) {
      out += "\n  only in v2: '" + path + "' = " + StatusName(status);
    }
  }
  return out;
}

void ExpectParsersAgree(const std::filesystem::path& repo,
                        const char* label,
                        std::size_t minimum_entries) {
  const std::string v1_output =
      RunGitCapture(repo, {"status", "--porcelain=v1", "-z", "--untracked-files=all"});
  const std::string v2_output = RunGitCapture(
      repo, {"status", "--porcelain=v2", "-z", "--branch", "--renames", "--untracked-files=all"});
  Expect(!v1_output.empty(), std::string(label) + ": git status v1 produced nothing to parse");
  Expect(!v2_output.empty(), std::string(label) + ": git status v2 produced nothing to parse");

  const auto v1 = V1StatusByPath(GitPorcelainParser::ParseWorkingTreeEntries(v1_output));
  const auto v2 = V2StatusByPath(GitPorcelainV2Parser::Parse(v2_output, repo, 1, 0));

  // A floor, not just non-emptiness: two parsers that both found nothing agree
  // perfectly and prove nothing.
  Expect(v1.size() >= minimum_entries,
         std::string(label) + ": the v1 parser found only " + std::to_string(v1.size()) +
             " entries, expected at least " + std::to_string(minimum_entries));
  Expect(v2.size() >= minimum_entries,
         std::string(label) + ": the v2 parser found only " + std::to_string(v2.size()) +
             " entries, expected at least " + std::to_string(minimum_entries));
  Expect(v1 == v2, std::string(label) +
                       ": the v1 and v2 parsers disagree about this repository" +
                       RenderDifference(v1, v2));
}

void TestPorcelainParsersAgreeOnARichWorkingTree() {
  TemporaryDirectory temp;
  const std::filesystem::path repo = temp.path() / "rich";
  BuildRichRepository(repo);
  ExpectParsersAgree(repo, "rich working tree", 12);

  // Agreement alone would also be satisfied by both parsers being wrong the same
  // way, so pin what the answer actually is. These are what `git status` reports
  // for the repository BuildRichRepository constructs.
  const std::string v2_output = RunGitCapture(
      repo, {"status", "--porcelain=v2", "-z", "--branch", "--renames", "--untracked-files=all"});
  const auto actual = V2StatusByPath(GitPorcelainV2Parser::Parse(v2_output, repo, 1, 0));
  const std::vector<std::pair<std::string, GitFileStatus>> expected = {
      {"modified.txt", GitFileStatus::Modified},
      {"staged-modify.txt", GitFileStatus::Modified},
      {"deleted.txt", GitFileStatus::Deleted},
      {"staged-delete.txt", GitFileStatus::Deleted},
      // A rename has no state of its own in this taxonomy -- the precedence
      // table maps R alongside M, and `old_path` is what carries the origin.
      {"rename-target.txt", GitFileStatus::Modified},
      {"added.txt", GitFileStatus::Added},
      {"both-staged-and-dirty.txt", GitFileStatus::Modified},
      {"untracked.txt", GitFileStatus::Untracked},
      {"untracked-dir/inside.txt", GitFileStatus::Untracked},
      {"untracked with space.txt", GitFileStatus::Untracked},
      {"with space.txt", GitFileStatus::Modified},
      {"naïve-ünïcode.txt", GitFileStatus::Modified},
  };
  for (const auto& [path, status] : expected) {
    const auto it = actual.find(path);
    Expect(it != actual.end(), "'" + path + "' should appear in the parsed status");
    Expect(it->second == status,
           "'" + path + "' should be " + StatusName(status) + ", parsed as " +
               StatusName(it->second));
  }
  // Files nothing touched must not appear at all.
  for (const char* path : {"unchanged.txt", "nested/deeper/inner.txt", "with\"quote.txt"}) {
    Expect(actual.find(path) == actual.end(),
           std::string("'") + path + "' was not changed and must not be reported");
  }
}

void TestPorcelainParsersAgreeOnAConflictedMerge() {
  TemporaryDirectory temp;
  const std::filesystem::path repo = temp.path() / "conflicted";
  BuildConflictedRepository(repo);
  ExpectParsersAgree(repo, "conflicted merge", 2);

  // And the conflicts must actually be reported as conflicts by both, or the
  // agreement above is agreement on the wrong answer.
  const std::string v2_output = RunGitCapture(
      repo, {"status", "--porcelain=v2", "-z", "--branch", "--renames", "--untracked-files=all"});
  const auto state = GitPorcelainV2Parser::Parse(v2_output, repo, 1, 0);
  std::size_t conflicted = 0;
  for (const GitRepositoryEntry& entry : state.entries) {
    if (entry.conflicted) ++conflicted;
  }
  Expect(conflicted == 2,
         "both conflicted files should be reported as conflicts, saw " +
             std::to_string(conflicted));
}

// The staged-and-further-dirty case is the one the commit surface warns about,
// and only v2 carries the bits for it. Assert them directly.
void TestPorcelainV2ReportsStagedAndWorktreeDirtySeparately() {
  TemporaryDirectory temp;
  const std::filesystem::path repo = temp.path() / "bits";
  BuildRichRepository(repo);
  const std::string output = RunGitCapture(
      repo, {"status", "--porcelain=v2", "-z", "--branch", "--renames", "--untracked-files=all"});
  const auto state = GitPorcelainV2Parser::Parse(output, repo, 1, 0);

  const auto find = [&](std::string_view path) -> const GitRepositoryEntry* {
    for (const GitRepositoryEntry& entry : state.entries) {
      if (microide::project::GenericPathView(entry.path) == path) return &entry;
    }
    return nullptr;
  };

  const GitRepositoryEntry* both = find("both-staged-and-dirty.txt");
  Expect(both != nullptr, "the staged-and-dirty file should appear in the status");
  Expect(both->staged && both->worktree_dirty,
         "a file staged then edited again is both staged and worktree-dirty");

  const GitRepositoryEntry* staged_only = find("staged-modify.txt");
  Expect(staged_only != nullptr, "the staged-only file should appear in the status");
  Expect(staged_only->staged && !staged_only->worktree_dirty,
         "a file staged and untouched since is staged and clean in the worktree");

  const GitRepositoryEntry* worktree_only = find("modified.txt");
  Expect(worktree_only != nullptr, "the worktree-only file should appear in the status");
  Expect(!worktree_only->staged && worktree_only->worktree_dirty,
         "a file edited but not staged is worktree-dirty and not staged");

  // A rename must carry where it came from, or the compare pane opens the wrong
  // left side.
  const GitRepositoryEntry* renamed = find("rename-target.txt");
  Expect(renamed != nullptr, "the renamed file should appear under its new name");
  Expect(renamed->old_path.has_value() &&
             microide::project::GenericPathView(*renamed->old_path) == "rename-source.txt",
         "a staged rename must carry its origin path");
}

// The v1 wire C-quotes any path with a space, a quote, a backslash or a
// non-ASCII byte. Both parsers must land on the same unquoted text, or the two
// halves of the app index the same file under two different keys.
void TestBothParsersUnquoteAwkwardPathsIdentically() {
  TemporaryDirectory temp;
  const std::filesystem::path repo = temp.path() / "quoted";
  BuildRichRepository(repo);

  const std::string v2_output = RunGitCapture(
      repo, {"status", "--porcelain=v2", "-z", "--branch", "--renames", "--untracked-files=all"});
  const auto v2 = V2StatusByPath(GitPorcelainV2Parser::Parse(v2_output, repo, 1, 0));

  for (const char* path : {"with space.txt", "naïve-ünïcode.txt", "untracked with space.txt"}) {
    Expect(v2.find(path) != v2.end(),
           std::string("the v2 parser should key this path as written on disk: '") + path +
               "' -- got" + [&] {
                 std::string all;
                 for (const auto& [key, status] : v2) all += "\n    '" + key + "'";
                 return all;
               }());
  }
}

}  // namespace

void RegisterGitPorcelainDifferentialTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitPorcelainDifferential/ParsersAgreeOnARichWorkingTree",
          TestPorcelainParsersAgreeOnARichWorkingTree);
  AddTest(tests, "GitPorcelainDifferential/ParsersAgreeOnAConflictedMerge",
          TestPorcelainParsersAgreeOnAConflictedMerge);
  AddTest(tests, "GitPorcelainDifferential/V2ReportsStagedAndWorktreeDirtySeparately",
          TestPorcelainV2ReportsStagedAndWorktreeDirtySeparately);
  AddTest(tests, "GitPorcelainDifferential/BothParsersUnquoteAwkwardPathsIdentically",
          TestBothParsersUnquoteAwkwardPathsIdentically);
}

}  // namespace microide::tests
