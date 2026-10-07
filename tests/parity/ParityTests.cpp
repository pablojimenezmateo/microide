// Local/remote parity (dev-docs/design/remote-projects.md § 10.1, Groundwork G11):
// each scenario runs against the same tree opened locally and opened through the
// loopback non-local locality (tests/parity/LoopbackLocality.h), and what the user
// would see must be equal. A row that cannot be equal yet is in ParityKnownGaps.h
// with what removes it.

#include <filesystem>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "parity/ParityHarness.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {

namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using parity::Outcome;
using parity::Scenario;
using parity::Tree;

std::string_view SectionName(workspace::GitSidebarEntry::Section section) {
  switch (section) {
    case workspace::GitSidebarEntry::Section::Conflicts:
      return "conflicts";
    case workspace::GitSidebarEntry::Section::Staged:
      return "staged";
    case workspace::GitSidebarEntry::Section::Changed:
      return "changed";
    case workspace::GitSidebarEntry::Section::Untracked:
      return "untracked";
    case workspace::GitSidebarEntry::Section::Outgoing:
      return "outgoing";
  }
  return "?";
}

// An editor save lands in the tree the project's processes see. A save that
// bypassed the project's write gate would leave the host's bytes unchanged.
Scenario SaveReachesTheTree() {
  return Scenario{
      .name = "Parity/SaveReachesTheTree",
      .build = [](const std::filesystem::path& root,
                  bool) { WriteFile(root / "notes.txt", "alpha\n"); },
      .run =
          [](WorkspaceShell& shell, const Tree& tree, Outcome& outcome) {
            WorkspaceShellTestAccess::OpenFile(shell, tree.root / "notes.txt");
            auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
            editor.MoveCursorTo(0, 5);
            editor.InsertText("_beta");
            const bool saved = WorkspaceShellTestAccess::SaveTab(
                shell, WorkspaceShellTestAccess::ActiveTabIndex(shell));
            outcome.Add("save", "ok", saved ? "yes" : "no");
            outcome.Add("editor", "dirty", editor.dirty() ? "yes" : "no");
            outcome.Add("bytes", "notes.txt", ReadFile(tree.host_root / "notes.txt"));
          },
      .writes = true,
  };
}

// The git sidebar shows the host's working tree. The mirror has no `.git`
// (design § 6.2), so this is where every reader of a LOCAL `.git` shows up.
Scenario GitSidebarShowsTheWorkingTree() {
  return Scenario{
      .name = "Parity/GitSidebarShowsTheWorkingTree",
      .build =
          [](const std::filesystem::path& root, bool is_mirror) {
            if (!is_mirror) {
              InitializeGitRepo(root);
              WriteFile(root / "tracked.txt", "one\n");
              CommitAll(root, "base", "parity git fixture");
            }
            WriteFile(root / "tracked.txt", "one\ntwo\n");
            WriteFile(root / "untracked.txt", "new\n");
          },
      .run =
          [](WorkspaceShell& shell, const Tree& tree, Outcome& outcome) {
            WorkspaceShellTestAccess::ShowGitSidebar(shell);
            // Content is asserted, not timing: the deadline only bounds a hang.
            (void)WaitUntil(
                [&shell] { return !WorkspaceShellTestAccess::GitSidebarRefreshing(shell); },
                std::chrono::seconds(10), std::chrono::milliseconds(10),
                [&shell] { WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell); });
            // The local run is the reference, so it must be RIGHT on its own: while
            // this row is a known gap, a broken local run would still "differ" and
            // read as the expected gap.
            if (tree.locality == parity::Locality::kLocal) {
              Expect(WorkspaceShellTestAccess::GitSidebarEntries(shell).size() == 2,
                     "parity reference: the local git sidebar lists both changes");
            }
            for (const auto& entry : WorkspaceShellTestAccess::GitSidebarEntries(shell)) {
              outcome.Add("git-entry", entry.relative_path, SectionName(entry.section));
            }
            for (const std::string& line : WorkspaceShellTestAccess::GitSidebarSummaryLines(shell)) {
              outcome.Add("git-summary", "", line);
            }
          },
      .spawns = true,
  };
}

}  // namespace

void RegisterParityTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Parity/SaveReachesTheTree",
          [] { parity::ExpectParity(SaveReachesTheTree()); });
  AddTest(tests, "Parity/GitSidebarShowsTheWorkingTree",
          [] { parity::ExpectParity(GitSidebarShowsTheWorkingTree()); });

  // Positive controls: the runner must be able to fail. A parity suite that cannot
  // tell a leaked host path from a correct one, or that passes when nothing went
  // through the loopback, is green and worthless (validation-traps.md).
  AddTest(tests, "Parity/Control/LeakedHostPathFails", [] {
    const Scenario leak{
        .name = "Parity/Control/LeakedHostPath",
        .build = [](const std::filesystem::path& root, bool) { WriteFile(root / "a.txt", "a\n"); },
        .run = [](WorkspaceShell&, const Tree& tree,
                  Outcome& outcome) { outcome.Add("path", "shown", (tree.host_root / "a.txt").string()); },
    };
    const std::string failure = parity::CheckParity(leak);
    Expect(failure.find("+ loopback: path | shown | ") != std::string::npos,
           "a host path shown to the user must fail parity, got: " + failure);
  });
  AddTest(tests, "Parity/Control/UnwiredLoopbackFails", [] {
    const Scenario unwired{
        .name = "Parity/Control/UnwiredLoopback",
        .build = [](const std::filesystem::path& root, bool) { WriteFile(root / "a.txt", "a\n"); },
        .run = [](WorkspaceShell&, const Tree&, Outcome& outcome) { outcome.Add("k", "", "v"); },
        .spawns = true,
    };
    Expect(parity::CheckParity(unwired).find("saw no spawn") != std::string::npos,
           "a spawning scenario with no loopback spawn must fail as vacuous");
  });
  AddTest(tests, "Parity/Control/EmptyOutcomeFails", [] {
    const Scenario empty{
        .name = "Parity/Control/EmptyOutcome",
        .build = [](const std::filesystem::path& root, bool) { WriteFile(root / "a.txt", "a\n"); },
        .run = [](WorkspaceShell&, const Tree&, Outcome&) {},
    };
    Expect(parity::CheckParity(empty).find("observed nothing") != std::string::npos,
           "a scenario that records nothing must fail as vacuous");
  });
}

}  // namespace microide::tests
