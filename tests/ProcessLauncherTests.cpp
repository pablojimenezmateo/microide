#include "TestSupport.h"
#include "workspace/git/MergeResolverContext.h"
#include "project/GitMetadataSource.h"

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
#include "workspace/debug/LaunchConfig.h"
#include "workspace/debug/WorkspaceDapManager.h"
#include "workspace/lsp/WorkspaceLspManager.h"

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
  // The root travels as the working directory, which a launcher maps onto the
  // machine that runs git; a `-C <root>` in argv is invisible to it and would name
  // the editor's copy of the tree on the host.
  Expect(!has("-C"), "the repository root is not a path inside argv");
  Expect(launcher.run_cwds.size() == 1 &&
             launcher.run_cwds.front() == std::filesystem::path("/nonexistent/project"),
         "the repository root is the working directory the launcher is asked for");
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

// A launcher that runs processes somewhere whose `.git` this machine cannot stat
// is also the project's git metadata source (TD-2026-10-06-319). Without that,
// every validity probe stats the local copy of the tree -- a mirror, with no
// `.git` -- and answers "not a repository" before git is ever asked.
class UnknownHostLauncher final : public platform::ProcessLauncher,
                                  public project::GitMetadataSource {
 public:
  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override { return argv; }
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override { return cwd; }
  platform::SubprocessResult Run(std::vector<std::string>, platform::SubprocessOptions) const override {
    return {};
  }
  bool is_local() const override { return false; }
  std::string_view description() const override { return "unknown-host"; }
  project::GitAvailability Availability(const std::filesystem::path&) const override {
    return project::GitAvailability::Unknown;
  }
  std::optional<std::filesystem::path> ReadableGitDirectory(
      const std::filesystem::path&) const override {
    return std::nullopt;
  }
};

void TestGitMetadataComesFromTheLaunchersHost() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");

  Expect(&project::GitMetadataFor(platform::LocalProcessLauncher()) ==
             &project::LocalGitMetadataSource(),
         "the local launcher's trees are answered by a local stat");
  Expect(project::GitRepository(root, platform::LocalProcessLauncher()).IsValid(),
         "a local tree with a .git is a repository");

  const UnknownHostLauncher host;
  Expect(&project::GitMetadataFor(host) == static_cast<const project::GitMetadataSource*>(&host),
         "a launcher that is a metadata source answers for its own trees");
  Expect(!project::GitRepository(root, host).IsValid(),
         "a LOCAL .git says nothing about a remote host: its source, still Unknown, decides");
  Expect(project::GitMetadataFor(host).Availability(root) == project::GitAvailability::Unknown,
         "and unknown is reported as unknown, not as not-a-repository");
}

// The merge resolver's "Incoming" caption used to read MERGE_HEAD on the shell
// thread; the refresh records it instead, through the same source.
void TestStatusRefreshRecordsThePendingMergeHead() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");
  const std::string oid = "0123456789abcdef0123456789abcdef01234567";
  WriteFile(root / ".git" / "MERGE_HEAD", oid + "\n");
  ScriptedProcessLauncher clean;
  clean.standing_response.exit_code = 0;
  const project::GitRepository repo(root, clean);
  const project::GitRepositoryState state =
      project::BuildGitRepositoryStateFromStatus(repo, root, 1, 1);
  Expect(state.operation_state == project::GitOperationStateKind::Merge,
         "MERGE_HEAD marks a merge in progress");
  Expect(state.pending_merge_head == oid, "and the refresh records its id");
  const workspace::MergeResolverLabels labels =
      workspace::BuildMergeResolverLabels(root, root / "a.txt", state);
  Expect(labels.incoming_label.find("0123456") != std::string::npos,
         "the Incoming caption shows the abbreviated merge head from the state, got: " +
             labels.incoming_label);
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
  // Appended rather than chained with operator+: GCC 13's -Warray-bounds misreads
  // the chained temporary's SSO buffer and warns on every build of this file.
  truncated.standing_response.stdout_text = "1 .M N... 100644 100644 100644 ";
  truncated.standing_response.stdout_text += "0000000 0000000 changed.txt";
  truncated.standing_response.stdout_text.push_back('\0');
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

// Slice 4: the two long-lived spawns. A language server and a debug adapter do not
// go through `Run`; they resolve their argv through the launcher and start the
// process themselves, so `resolved_argvs` is what shows which launcher they asked.
// `true` really starts and exits at once — nothing here waits on it.
void TestLanguageServerStartsThroughTheProjectsLauncher() {
  ScriptedProcessLauncher launcher;
  {
    workspace::LspManager manager;
    manager.RegisterServer({"scripted"}, launcher, {"true"}, "file:///tmp", /*cwd=*/{},
                           /*eager_start=*/true);
  }
  Expect(!launcher.resolved_argvs.empty() && launcher.resolved_argvs.front().front() == "true",
         "the language server's argv was resolved through the launcher it was registered "
         "with, not a local one the client reached for");

  // Re-registering the same server under a DIFFERENT launcher is a different
  // server — the project moved machines — so it must restart on the new one
  // rather than be skipped as an unchanged registration.
  ScriptedProcessLauncher first;
  ScriptedProcessLauncher second;
  {
    workspace::LspManager manager;
    manager.RegisterServer({"scripted"}, first, {"true"}, "file:///tmp", {}, true);
    manager.RegisterServer({"scripted"}, second, {"true"}, "file:///tmp", {}, true);
  }
  Expect(!second.resolved_argvs.empty(),
         "a re-registration that changes only the launcher restarts the server on it");
}

void TestDebugAdapterStartsThroughTheProjectsLauncher() {
  ScriptedProcessLauncher launcher;
  {
    workspace::DapManager manager;
    manager.RegisterAdapter("scripted", launcher, {"true"});
    workspace::LaunchConfig config;
    config.type = "scripted";
    config.request = "launch";
    (void)manager.StartSession(config, workspace::DebugSession::Callbacks{});
  }
  Expect(!launcher.resolved_argvs.empty() && launcher.resolved_argvs.front().front() == "true",
         "the debug adapter's argv was resolved through the launcher it was registered "
         "with, not a local one the client reached for");
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
  AddTest(tests, "ProcessLauncher/GitMetadataComesFromTheLaunchersHost",
          TestGitMetadataComesFromTheLaunchersHost);
  AddTest(tests, "ProcessLauncher/StatusRefreshRecordsThePendingMergeHead",
          TestStatusRefreshRecordsThePendingMergeHead);
  AddTest(tests, "ProcessLauncher/BlameRunsThroughTheRequestsLauncher",
          TestBlameRunsThroughTheRequestsLauncher);
  AddTest(tests, "ProcessLauncher/LanguageServerStartsThroughTheProjectsLauncher",
          TestLanguageServerStartsThroughTheProjectsLauncher);
  AddTest(tests, "ProcessLauncher/DebugAdapterStartsThroughTheProjectsLauncher",
          TestDebugAdapterStartsThroughTheProjectsLauncher);
}

}  // namespace microide::tests
