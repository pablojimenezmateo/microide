#include "TestSupport.h"

#include "ScriptedProcessLauncher.h"

#include "platform/ProcessLauncher.h"
#include "project/CommitWorkflowChecks.h"
#include "project/GitBlameService.h"
#include "project/GitBranchOperations.h"
#include "project/GitCommitExecutor.h"
#include "project/GitCompareService.h"
#include "project/GitRepository.h"
#include "project/GitStatusRefresh.h"
#include "project/GitStatusService.h"

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {


void TestScriptedLauncherDrivesGitWithoutAGitBinary() {
  ScriptedProcessLauncher launcher;
  launcher.standing_response.stdout_text = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef\n";
  const project::GitRepository repo("/nonexistent/project", launcher);

  const auto result = repo.Execute({"rev-parse", "--verify", "HEAD"});
  Expect(result.success(), "the scripted launcher's exit code is what the repository reports");
  Expect(result.output.find("deadbeef") != std::string::npos,
         "the scripted launcher's stdout is what the repository reports");
  Expect(launcher.runs.size() == 1, "exactly one git invocation was issued");

  // The argv the repository builds is a contract of its own: `--no-optional-locks`
  // keeps a read command off .git/index.lock, and `--literal-pathspecs` stops a file
  // whose name begins with git pathspec magic from being interpreted as a pattern.
  // Both were previously unobservable without running git and inspecting side effects.
  const std::vector<std::string>& argv = launcher.runs.front();
  Expect(!argv.empty() && argv.front() == "git", "the program is git");
  const auto has = [&](std::string_view flag) {
    for (const std::string& word : argv) {
      if (word == flag) {
        return true;
      }
    }
    return false;
  };
  Expect(has("--no-optional-locks"),
         "every git command suppresses the optional index refresh");
  Expect(has("--literal-pathspecs"), "every git command forces literal pathspecs");
  Expect(has("-C"), "the repository root is passed with -C rather than a chdir");
  Expect(argv.back() == "HEAD" && argv[argv.size() - 2] == "--verify",
         "the caller's arguments come last, in order");
}

void TestScriptedLauncherReportsFailureThrough() {
  ScriptedProcessLauncher launcher;
  launcher.standing_response.exit_code = 128;
  launcher.standing_response.stdout_text = "fatal: not a git repository";
  const project::GitRepository repo("/nonexistent/project", launcher);

  Expect(!repo.ExecuteSucceeds({"status"}),
         "a non-zero scripted exit code must surface as failure");
}


// The cases REAL git cannot be made to produce on demand, which is the whole
// reason the seam is worth having. Each one used to be indistinguishable from a
// clean working tree.
void TestGitNotInstalledIsNotACleanWorkingTree() {
  // `execvp` reports ENOENT as exit 127, so this is every git call on a machine
  // with no git installed.
  const ScriptedProcessLauncher launcher = ScriptedProcessLauncher::MissingProgram();
  const project::GitRepository repo("/nonexistent/project", launcher);

  const auto entries = repo.GetWorkingTreeEntries();
  Expect(!entries.has_value(),
         "git failing to run is NO ANSWER, not an answer of 'nothing changed' — the "
         "conflict review reported 'no conflicts' for exactly this");
}

void TestCleanWorkingTreeIsAnAnswer() {
  // git ran and said nothing, which is what a clean tree looks like. The empty
  // case has to stay distinguishable from the failure above, or the fix traded
  // one wrong answer for another.
  ScriptedProcessLauncher launcher;
  launcher.standing_response.exit_code = 0;
  launcher.standing_response.stdout_text = "";
  const project::GitRepository repo("/nonexistent/project", launcher);

  const auto entries = repo.GetWorkingTreeEntries();
  Expect(entries.has_value(), "a successful status is an answer");
  Expect(entries->empty(), "and the answer is that the tree is clean");
}

void TestTruncatedStatusIsNotACompleteChangeList() {
  // Output that hit the capture ceiling carries REAL entries — just not all of
  // them. Reporting a prefix as the whole list is the dangerous direction: the
  // user resolves what they are shown and believes they are finished.
  ScriptedProcessLauncher launcher;
  launcher.standing_response.exit_code = 0;
  launcher.standing_response.stdout_text = std::string("UU conflicted.txt") + '\0';
  launcher.standing_response.truncated = true;
  const project::GitRepository repo("/nonexistent/project", launcher);

  Expect(!repo.GetWorkingTreeEntries().has_value(),
         "a truncated status cannot be presented as the complete set of changes");
}


// The sidebar's status refresh, driven through a scripted git. Before
// `BuildGitRepositoryStateFromStatus` was split out of `GitRepositoryService`,
// none of these branches was reachable from a test: the service's own seam
// substitutes the whole step and returns a finished state, so it could exercise
// what the sidebar does WITH a state and never how git's output becomes one.
const ScriptedProcessLauncher& clean_launcher_probe() {
  static const ScriptedProcessLauncher launcher;
  return launcher;
}

void TestStatusRefreshDistinguishesItsFailures() {
  // `IsValid()` is a stat for the `.git` marker, so the fixture needs a marker and
  // nothing else — no `git init`, and therefore no git binary.
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");
  const auto build = [&root](const ScriptedProcessLauncher& launcher) {
    const project::GitRepository repo(root, launcher);
    return project::BuildGitRepositoryStateFromStatus(repo, root, /*generation=*/7,
                                                      /*refreshed_at_ms=*/1234);
  };

  // And the marker's absence is its own answer, distinct from every failure below.
  {
    const project::GitRepository not_a_repo(temp_dir.path() / "plain", clean_launcher_probe());
    const project::GitRepositoryState state = project::BuildGitRepositoryStateFromStatus(
        not_a_repo, temp_dir.path() / "plain", 7, 1234);
    Expect(state.refresh_error.category == project::GitRefreshErrorCategory::NotARepo,
           "a directory with no .git marker is reported as not a repository");
    Expect(!state.repo_available, "and as having no repository available");
  }

  // git ran and reported nothing: a clean repository, and NOT an error.
  ScriptedProcessLauncher clean;
  clean.standing_response.exit_code = 0;
  const project::GitRepositoryState clean_state = build(clean);
  Expect(clean_state.refresh_error.category == project::GitRefreshErrorCategory::None,
         "a clean status is not an error");
  Expect(!clean_state.stale, "and is not stale");
  Expect(clean_state.generation == 7 && clean_state.refreshed_at_ms == 1234,
         "the caller's generation and clock reading are stamped through");

  // git could not run at all. The change list must not read as empty-and-current.
  const project::GitRepositoryState missing = build(ScriptedProcessLauncher::MissingProgram());
  Expect(missing.refresh_error.category != project::GitRefreshErrorCategory::None,
         "git failing to run is reported as a refresh error");
  Expect(missing.stale, "and leaves the state marked stale");

  // git exited 0 but its output was cut at the capture ceiling. This is the
  // dangerous one: the entries that came back are REAL, so a parse succeeds and
  // the result looks like an ordinary, complete change list.
  ScriptedProcessLauncher truncated;
  truncated.standing_response.exit_code = 0;
  truncated.standing_response.stdout_text = std::string("1 .M N... 100644 100644 100644 ") +
                                            "0000000 0000000 changed.txt" + '\0';
  truncated.standing_response.truncated = true;
  const project::GitRepositoryState partial = build(truncated);
  Expect(partial.stale, "a truncated status is stale, not a complete change list");
  Expect(partial.refresh_error.category != project::GitRefreshErrorCategory::None,
         "and says why, rather than showing a prefix as the whole truth");
}


// The seam only means anything if the service actually USES the launcher it was
// handed. Before TD-2026-09-22-301's first slice, every one of these functions
// reached for `LocalProcessLauncher()` internally, so a project could not choose
// where its own git ran — and nothing would have noticed a reintroduction.
void TestStatusServiceRunsThroughTheLauncherItIsGiven() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");  // the marker, not a real repo

  ScriptedProcessLauncher launcher;
  launcher.standing_response.exit_code = 0;
  Expect(project::GitStageAll(root, launcher), "the staging call reports the scripted success");
  Expect(!launcher.runs.empty(),
         "the service ran git through the launcher it was given, not a local one it "
         "reached for itself");
  Expect(launcher.runs.front().front() == "git", "and the program it ran was git");

  // A failure from that launcher is the service's answer too — no silent fallback
  // to a local run.
  ScriptedProcessLauncher failing = ScriptedProcessLauncher::MissingProgram();
  Expect(!project::GitStageAll(root, failing), "a launcher that cannot run git fails the call");
  Expect(!failing.runs.empty(), "having actually been asked");
}

// Slice 2 of the same TD: every read-side git query (the compare/merge surfaces,
// the review verbs, the branch and commit pickers) now takes the launcher too.
// Nine `LocalProcessLauncher()` constructions lived inside GitCompareService, and
// a reintroduced one leaves the scripted launcher with no recorded run for that
// entry point — which is exactly what this walks.
void TestCompareServiceRunsThroughTheLauncherItIsGiven() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");  // the marker, not a real repo
  const std::filesystem::path file = root / "a.txt";

  // Every entry point, each against its own launcher, so a function that reached
  // for a local one internally shows up as an EMPTY run list rather than being
  // masked by its neighbours' runs.
  const auto ran = [&](std::string_view what, auto&& invoke) {
    ScriptedProcessLauncher launcher;
    invoke(launcher);
    Expect(!launcher.runs.empty(), std::string(what) + " ran git through the launcher it was "
                                                       "given, not a local one it reached for");
    Expect(launcher.runs.front().front() == "git",
           std::string(what) + " ran git, not some other program");
  };

  ran("CollectGitFileHistory", [&](const ScriptedProcessLauncher& l) {
    (void)project::CollectGitFileHistory(root, l, file);
  });
  ran("CollectGitRecentCommits", [&](const ScriptedProcessLauncher& l) {
    (void)project::CollectGitRecentCommits(root, l, 10);
  });
  ran("CollectGitBranches",
      [&](const ScriptedProcessLauncher& l) { (void)project::CollectGitBranches(root, l); });
  ran("ReadGitFileAtCommit", [&](const ScriptedProcessLauncher& l) {
    (void)project::ReadGitFileAtCommit(root, l, file, "HEAD");
  });
  ran("ResolveGitBaseReference",
      [&](const ScriptedProcessLauncher& l) { (void)project::ResolveGitBaseReference(root, l); });
  ran("CollectGitBranchOutgoingFiles", [&](const ScriptedProcessLauncher& l) {
    (void)project::CollectGitBranchOutgoingFiles(root, l, "main");
  });
  ran("CollectGitWorkingTreeDiffFiles", [&](const ScriptedProcessLauncher& l) {
    (void)project::CollectGitWorkingTreeDiffFiles(root, l, "main");
  });
  ran("CollectGitCommitChangedFiles", [&](const ScriptedProcessLauncher& l) {
    (void)project::CollectGitCommitChangedFiles(root, l, "HEAD");
  });
  ran("GitRevisionBlobCache::Prefetch", [&](const ScriptedProcessLauncher& l) {
    project::GitRevisionBlobCache cache;
    cache.Prefetch(root, l, {"HEAD"}, {file});
  });
}

// Slice 3: the WRITE side — branch switch/create, fetch/pull/push, stash, the
// commit itself and the staged-diff pre-checks — plus the HEAD resolution that
// `GitRepository::ResolveHeadId` used to send to the local launcher whatever the
// repository it was called on had been constructed with.
void TestWriteSideRunsThroughTheLauncherItIsGiven() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");  // the marker, not a real repo

  const auto ran = [&](std::string_view what, auto&& invoke) {
    ScriptedProcessLauncher launcher;
    invoke(launcher);
    Expect(!launcher.runs.empty(), std::string(what) + " ran git through the launcher it was "
                                                       "given, not a local one it reached for");
  };

  ran("SwitchGitBranch", [&](const ScriptedProcessLauncher& l) {
    (void)project::SwitchGitBranch(root, l, "main");
  });
  ran("CreateGitBranch", [&](const ScriptedProcessLauncher& l) {
    (void)project::CreateGitBranch(root, l, "topic");
  });
  ran("RunGitRemoteOperation", [&](const ScriptedProcessLauncher& l) {
    (void)project::RunGitRemoteOperation(root, l, project::GitRemoteOperationKind::Fetch);
  });
  ran("StashGitChanges", [&](const ScriptedProcessLauncher& l) {
    (void)project::StashGitChanges(root, l, "", false);
  });
  ran("PopGitStash", [&](const ScriptedProcessLauncher& l) { (void)project::PopGitStash(root, l); });
  ran("ExecuteGitCommit", [&](const ScriptedProcessLauncher& l) {
    (void)project::ExecuteGitCommit(root, l, "subject", "", project::CommitOperationKind::Create);
  });

  project::GitRepositoryState state;
  state.repository_root = root;
  state.repo_available = true;
  ran("BuildCommitStagedSummary", [&](const ScriptedProcessLauncher& l) {
    (void)project::BuildCommitStagedSummary(state, l);
  });
  ran("StagedDiffContainsConflictMarkers", [&](const ScriptedProcessLauncher& l) {
    (void)project::StagedDiffContainsConflictMarkers(root, l);
  });
  ran("GitRepository::ResolveHeadId", [&](const ScriptedProcessLauncher& l) {
    (void)project::GitRepository(root, l).ResolveHeadId();
  });
}

// Blame is the one that used to be HELD rather than passed: the service carried a
// launcher behind a `SetLauncher` nothing called, so every blame ran locally. It
// rides on the request now. Only a liveness wait here — the scripted launcher
// answers instantly, so the deadline is a bound on "never ran", not a timing claim.
void TestBlameRunsThroughTheRequestsLauncher() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");
  const std::filesystem::path file = root / "a.txt";
  WriteFile(file, "one\ntwo\n");

  ScriptedProcessLauncher launcher;
  project::GitBlameService service;
  project::GitBlameRequest request;
  request.launcher = &launcher;
  request.root = root;
  request.absolute_path = file;
  request.visible_line_count = 2;
  request.total_line_count = 2;

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  service.Request(request);
  while (service.Snapshot(request).loading && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  service.Stop();  // joins nothing in flight after this; the worker is quiescent
  Expect(!launcher.runs.empty(),
         "a blame ran its git through the launcher on its request, not a held local one");
}

}  // namespace

void RegisterProcessLauncherTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProcessLauncher/ScriptedLauncherDrivesGitWithoutAGitBinary",
          TestScriptedLauncherDrivesGitWithoutAGitBinary);
  AddTest(tests, "ProcessLauncher/ScriptedLauncherReportsFailureThrough",
          TestScriptedLauncherReportsFailureThrough);
  AddTest(tests, "ProcessLauncher/GitNotInstalledIsNotACleanWorkingTree",
          TestGitNotInstalledIsNotACleanWorkingTree);
  AddTest(tests, "ProcessLauncher/CleanWorkingTreeIsAnAnswer", TestCleanWorkingTreeIsAnAnswer);
  AddTest(tests, "ProcessLauncher/TruncatedStatusIsNotACompleteChangeList",
          TestTruncatedStatusIsNotACompleteChangeList);
  AddTest(tests, "ProcessLauncher/StatusRefreshDistinguishesItsFailures",
          TestStatusRefreshDistinguishesItsFailures);
  AddTest(tests, "ProcessLauncher/StatusServiceRunsThroughTheLauncherItIsGiven",
          TestStatusServiceRunsThroughTheLauncherItIsGiven);
  AddTest(tests, "ProcessLauncher/CompareServiceRunsThroughTheLauncherItIsGiven",
          TestCompareServiceRunsThroughTheLauncherItIsGiven);
  AddTest(tests, "ProcessLauncher/WriteSideRunsThroughTheLauncherItIsGiven",
          TestWriteSideRunsThroughTheLauncherItIsGiven);
  AddTest(tests, "ProcessLauncher/BlameRunsThroughTheRequestsLauncher",
          TestBlameRunsThroughTheRequestsLauncher);
}

}  // namespace microide::tests
