// Git: Switch Worktree…: what `git worktree list --porcelain -z` says
// (project::ParseGitWorktreeList) and the pick's rows built from it, without git.

#include "TestSupport.h"

#include "project/GitWorktrees.h"
#include "workspace/git/CommitPick.h"
#include "workspace/git/WorktreePick.h"

#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::GitWorktree;
using microide::project::GitWorktreeState;
using microide::project::ParseGitWorktreeList;
using microide::project::ParseGitWorktreeState;
using microide::workspace::BuildWorktreePickItems;
using microide::workspace::QuickPickItem;

// NUL-terminated lines; a record ends with an empty one.
std::string Porcelain(std::vector<std::vector<std::string_view>> records) {
  std::string out;
  for (const auto& record : records) {
    for (const std::string_view line : record) {
      out.append(line);
      out.push_back('\0');
    }
    out.push_back('\0');
  }
  return out;
}

void TestWorktreeListParsesEveryKindOfEntry() {
  const std::vector<GitWorktree> worktrees = ParseGitWorktreeList(Porcelain({
      {"worktree /src/repo", "HEAD 1111111111111111111111111111111111111111", "branch refs/heads/main"},
      {"worktree /src/repo wt/agent one", "HEAD 2222222222222222222222222222222222222222",
       "branch refs/heads/feature/x", "locked agent at work"},
      {"worktree /src/detached", "HEAD 3333333333333333333333333333333333333333", "detached"},
      {"worktree /src/gone", "HEAD 4444444444444444444444444444444444444444", "detached",
       "prunable gitdir file points to non-existent location"},
      {"worktree /src/bare.git", "bare"},
  }));
  Expect(worktrees.size() == 5, "five records: " + std::to_string(worktrees.size()));
  if (worktrees.size() != 5) {
    return;
  }
  Expect(worktrees[0].path == "/src/repo" && worktrees[0].branch == "main" && !worktrees[0].locked,
         "the main tree and its branch's short name");
  Expect(worktrees[1].path == "/src/repo wt/agent one" && worktrees[1].branch == "feature/x" &&
             worktrees[1].locked,
         "a path with spaces is whole, a branch keeps its slashes, and a lock's reason is not the flag");
  Expect(worktrees[2].branch.empty() && worktrees[2].head.starts_with("3333"), "a detached tree has only a commit");
  Expect(worktrees[3].prunable, "a tree whose directory is gone");
  Expect(worktrees[4].bare && worktrees[4].head.empty(), "a bare entry");

  Expect(ParseGitWorktreeList("").empty() && ParseGitWorktreeList(std::string_view("\0\0", 2)).empty(),
         "nothing listed is nothing");
  // A last record cut short of its terminator still counts.
  std::string cut = "worktree /src/repo";
  cut.push_back('\0');
  cut += "branch refs/heads/main";
  const std::vector<GitWorktree> partial = ParseGitWorktreeList(cut);
  Expect(partial.size() == 1 && partial[0].branch == "main", "an unterminated record is kept");
}

void TestWorktreePickRowsOpenEachTreeWhereItIs() {
  const std::vector<GitWorktree> worktrees = {
      GitWorktree{.path = "/src/repo", .branch = "main", .head = "1111111111111111"},
      GitWorktree{.path = "/src/wt/agent one", .branch = "feature/x", .head = "2222222222222222", .locked = true},
      GitWorktree{.path = "/src/detached", .head = "3333333333333333"},
      GitWorktree{.path = "/src/gone", .head = "4444444444444444", .prunable = true},
      GitWorktree{.path = "/src/bare.git", .bare = true},
  };
  const std::vector<QuickPickItem> local = BuildWorktreePickItems(worktrees, "/src/repo/", "project-open", "");
  Expect(local.size() == 3, "a bare entry and a prunable one are not offered: " + std::to_string(local.size()));
  if (local.size() != 3) {
    return;
  }
  Expect(local[0].primary_label == "repo" && local[0].value == "project-open /src/repo",
         "a row is its directory and opens it: " + local[0].value);
  Expect(local[0].secondary_label == "main \xc2\xb7 /src \xc2\xb7 this project",
         "the open project's own tree says so: " + local[0].secondary_label);
  Expect(local[1].value == "project-open '/src/wt/agent one'",
         "a path with a space is quoted for the command line: " + local[1].value);
  Expect(local[1].secondary_label == "feature/x \xc2\xb7 /src/wt \xc2\xb7 locked", "a locked tree: " + local[1].secondary_label);
  Expect(local[2].secondary_label == "detached 3333333 \xc2\xb7 /src", "a detached tree shows its commit: " + local[2].secondary_label);
  Expect(local[1].search_text.find("feature/x") != std::string::npos, "rows filter by branch");

  const std::vector<QuickPickItem> remote =
      BuildWorktreePickItems(worktrees, "/src/detached", "remote-open", "dev@build:");
  // Quoted for the command line, which takes the quotes off again.
  Expect(remote.size() == 3 && remote[0].value == "remote-open 'dev@build:/src/repo'",
         "a host's tree opens on the host: " + (remote.empty() ? std::string() : remote[0].value));
  Expect(remote.size() == 3 && remote[2].secondary_label.ends_with("this project") &&
             !remote[0].secondary_label.ends_with("this project"),
         "the current tree is the host root's");
}

// What `git status --porcelain=v2 --branch -z` says of a tree: its entries
// counted (a rename's original path is part of its record, not another change)
// and the branch's distance from its upstream, when it has one.
void TestWorktreeStateCountsChangesAndReadsTheUpstream() {
  const auto status = [](std::initializer_list<std::string_view> records) {
    std::string out;
    for (const std::string_view record : records) {
      out.append(record);
      out.push_back('\0');
    }
    return out;
  };
  const GitWorktreeState busy = ParseGitWorktreeState(status({
      "# branch.oid 1111111111111111111111111111111111111111",
      "# branch.head agent",
      "# branch.upstream origin/agent",
      "# branch.ab +3 -1",
      "1 .M N... 100644 100644 100644 aaaa bbbb src/a.cpp",
      "2 R. N... 100644 100644 100644 aaaa bbbb R100 src/new name.cpp",
      "src/old name.cpp",
      "u UU N... 100644 100644 100644 100644 aaaa bbbb cccc src/conflict.cpp",
      "? notes.md",
      "! build/",
  }));
  Expect(busy == GitWorktreeState{.changed = 4, .has_upstream = true, .ahead = 3, .behind = 1},
         "four changes, three ahead and one behind: " + std::to_string(busy.changed) + " " +
             std::to_string(busy.ahead) + " " + std::to_string(busy.behind));
  const GitWorktreeState clean = ParseGitWorktreeState(status({
      "# branch.oid 1111111111111111111111111111111111111111",
      "# branch.head (detached)",
  }));
  Expect(clean == GitWorktreeState{}, "a clean detached tree: nothing changed, no upstream");
  Expect(ParseGitWorktreeState(status({"# branch.ab +x -1", "# branch.ab 3 1"})) == GitWorktreeState{},
         "a header that is not two signed counts is not believed");
  Expect(ParseGitWorktreeState("") == GitWorktreeState{}, "nothing said is nothing");

  // The pick puts what waits in a tree before where the tree is.
  const std::vector<GitWorktree> worktrees = {
      GitWorktree{.path = "/src/repo", .branch = "main", .head = "1111111111111111", .state = GitWorktreeState{}},
      GitWorktree{.path = "/src/wt/agent", .branch = "agent", .head = "2222222222222222", .state = busy},
      GitWorktree{.path = "/src/wt/behind", .branch = "old", .head = "3333333333333333",
                  .state = GitWorktreeState{.has_upstream = true, .behind = 2}},
  };
  const std::vector<QuickPickItem> rows = BuildWorktreePickItems(worktrees, "/elsewhere", "project-open", "");
  Expect(rows.size() == 3 && rows[0].secondary_label == "main \xc2\xb7 /src",
         "a clean tree level with its upstream adds nothing: " + (rows.empty() ? std::string() : rows[0].secondary_label));
  Expect(rows.size() == 3 &&
             rows[1].secondary_label == "agent \xc2\xb7 4 changed \xc2\xb7 3\xe2\x86\x91 \xc2\xb7 1\xe2\x86\x93 \xc2\xb7 /src/wt",
         "a tree with work in it says how much: " + (rows.size() < 2 ? std::string() : rows[1].secondary_label));
  Expect(rows.size() == 3 && rows[2].secondary_label == "old \xc2\xb7 2\xe2\x86\x93 \xc2\xb7 /src/wt",
         "a tree that is only behind: " + (rows.size() < 3 ? std::string() : rows[2].secondary_label));
}

// Git: Review Commit…: a row per recent commit, opening its review by full hash.
void TestCommitPickRowsReviewEachCommit() {
  const std::vector<microide::project::GitCommitEntry> commits = {
      {.hash = "1111111111111111111111111111111111111111", .short_hash = "1111111",
       .subject = "fix(merge): the boxes sit on the rows", .author = "agent", .relative_date = "2 hours ago"},
      {.hash = "2222222222222222222222222222222222222222", .short_hash = "2222222", .subject = "",
       .author = "", .relative_date = ""},
      {.hash = "", .short_hash = "", .subject = "no hash", .author = "x", .relative_date = "now"},
  };
  const std::vector<QuickPickItem> rows = microide::workspace::BuildCommitPickItems(commits);
  Expect(rows.size() == 2, "a row a commit, and none for an entry with no hash: " + std::to_string(rows.size()));
  if (rows.size() != 2) {
    return;
  }
  Expect(rows[0].value == "review-commit 1111111111111111111111111111111111111111" &&
             rows[0].primary_label == "fix(merge): the boxes sit on the rows" &&
             rows[0].secondary_label == "1111111 \xc2\xb7 2 hours ago \xc2\xb7 agent",
         "its subject, then hash, age and author: " + rows[0].secondary_label);
  Expect(rows[1].primary_label == "2222222" && rows[1].secondary_label == "2222222",
         "a commit with no subject is named by its hash");
  Expect(rows[0].search_text.find("agent") != std::string::npos && rows[0].search_text.find("fix(merge)") != std::string::npos,
         "rows filter by subject and author");
}

}  // namespace

void RegisterWorktreePickTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorktreePick/StateCountsChangesAndReadsTheUpstream",
          TestWorktreeStateCountsChangesAndReadsTheUpstream);
  AddTest(tests, "WorktreePick/CommitPickRowsReviewEachCommit", TestCommitPickRowsReviewEachCommit);
  AddTest(tests, "WorktreePick/ListParsesEveryKindOfEntry", TestWorktreeListParsesEveryKindOfEntry);
  AddTest(tests, "WorktreePick/RowsOpenEachTreeWhereItIs", TestWorktreePickRowsOpenEachTreeWhereItIs);
}

}  // namespace microide::tests
