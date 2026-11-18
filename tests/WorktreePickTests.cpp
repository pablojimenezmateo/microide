// Git: Switch Worktree…: what `git worktree list --porcelain -z` says
// (project::ParseGitWorktreeList) and the pick's rows built from it, without git.

#include "TestSupport.h"

#include "project/GitWorktrees.h"
#include "workspace/git/WorktreePick.h"

#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::GitWorktree;
using microide::project::ParseGitWorktreeList;
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

}  // namespace

void RegisterWorktreePickTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorktreePick/ListParsesEveryKindOfEntry", TestWorktreeListParsesEveryKindOfEntry);
  AddTest(tests, "WorktreePick/RowsOpenEachTreeWhereItIs", TestWorktreePickRowsOpenEachTreeWhereItIs);
}

}  // namespace microide::tests
