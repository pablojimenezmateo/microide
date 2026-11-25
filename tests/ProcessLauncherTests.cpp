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

void TestGitNotInstalledIsNotACleanWorkingTree() {
  const ScriptedProcessLauncher launcher = ScriptedProcessLauncher::MissingProgram();
  const project::GitRepository repo("/nonexistent/project", launcher);

  const auto entries = repo.GetWorkingTreeEntries();
  Expect(!entries.has_value(),
         "git failing to run is NO ANSWER, not an answer of 'nothing changed' — the "
         "conflict review reported 'no conflicts' for exactly this");
}

void TestCleanWorkingTreeIsAnAnswer() {
  ScriptedProcessLauncher launcher;
  launcher.standing_response.exit_code = 0;
  launcher.standing_response.stdout_text = "";
  const project::GitRepository repo("/nonexistent/project", launcher);

  const auto entries = repo.GetWorkingTreeEntries();
  Expect(entries.has_value(), "a successful status is an answer");
  Expect(entries->empty(), "and the answer is that the tree is clean");
}

void TestTruncatedStatusIsNotACompleteChangeList() {
  ScriptedProcessLauncher launcher;
  launcher.standing_response.exit_code = 0;
  using namespace std::string_literals;
  launcher.standing_response.stdout_text = "UU conflicted.txt\0"s;
  launcher.standing_response.truncated = true;
  const project::GitRepository repo("/nonexistent/project", launcher);

  Expect(!repo.GetWorkingTreeEntries().has_value(),
         "a truncated status cannot be presented as the complete set of changes");
}

const ScriptedProcessLauncher& clean_launcher_probe() {
  static const ScriptedProcessLauncher launcher;
  return launcher;
}

class UnknownHostLauncher final : public platform::ProcessLauncher,
                                  public project::GitMetadataSource {
 public:
  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override { return argv; }
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override { return cwd; }
  std::filesystem::path LocalPathFromHost(std::filesystem::path p) const override { return p; }
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

void TestWorkspaceFolderExpandsToTheHostRoot() {
  const ScriptedProcessLauncher local;
  const std::vector<std::string> plain = {"clangd", "--log=error"};
  Expect(platform::ExpandWorkspaceFolder(plain, "/m/p", local) == plain,
         "an argv with no placeholder is untouched");
  Expect(platform::ExpandWorkspaceFolder({"tool", "--root=${workspaceFolder}/build",
                                          "${workspaceFolder}"},
                                         "/m/p", local) ==
             std::vector<std::string>{"tool", "--root=/m/p/build", "/m/p"},
         "every occurrence expands, locally to the project root");

  class HostLauncher final : public platform::ProcessLauncher {
   public:
    std::vector<std::string> ResolveArgv(std::vector<std::string> a) const override { return a; }
    std::filesystem::path ResolveWorkingDirectory(std::filesystem::path p) const override {
      return p == "/m/p" ? std::filesystem::path("/h/p") : p;
    }
    std::filesystem::path LocalPathFromHost(std::filesystem::path p) const override { return p; }
    platform::SubprocessResult Run(std::vector<std::string>, platform::SubprocessOptions) const override {
      return {};
    }
    bool is_local() const override { return false; }
    std::string_view description() const override { return "host"; }
  } host;
  Expect(platform::ExpandWorkspaceFolder({"--root=${workspaceFolder}"}, "/m/p", host) ==
             std::vector<std::string>{"--root=/h/p"},
         "and remotely to the root as the host sees it");
}

void TestFirstAvailableCommandRunsTheFirstThatExists() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path tool = home / ".tools" / "say";
  WriteFile(tool, "#!/bin/sh\nprintf 'tool:%s|' \"$@\"\n");
  std::filesystem::permissions(tool, std::filesystem::perms::owner_all);
  const auto run = [&](const std::vector<std::vector<std::string>>& candidates) {
    std::vector<std::string> argv = {"env", "HOME=" + home.string()};
    const std::vector<std::string> resolver =
        platform::FirstAvailableCommand(candidates, "install the say tool");
    argv.insert(argv.end(), resolver.begin(), resolver.end());
    return platform::RunSubprocess(argv);
  };

  Expect(platform::FirstAvailableCommand({{"clangd", "--log=error"}}, "hint") ==
             std::vector<std::string>{"clangd", "--log=error"},
         "a single candidate is the command as it is");

  const auto from_path = run({{"microide-no-such-program", "x"}, {"printf", "%s|", "a b", "c"}});
  Expect(from_path.exit_code == 0 && from_path.stdout_text == "a b|c|",
         "a missing program is skipped and the next runs with its own arguments intact, got [" +
             from_path.stdout_text + "]");

  const auto from_home = run({{"/nonexistent/bin/say"}, {"~/.tools/say", "one", "two words"}});
  Expect(from_home.exit_code == 0 && from_home.stdout_text == "tool:one|tool:two words|",
         "a `~/` program is the user's home where it runs, got [" + from_home.stdout_text + "]");

  const auto first_wins = run({{"printf", "first"}, {"printf", "second"}});
  Expect(first_wins.stdout_text == "first", "the first that exists wins");

  const auto none = run({{"microide-no-such-program"}, {"/nonexistent/also-missing"}});
  Expect(none.exit_code == 127 && none.stderr_text.find("install the say tool") != std::string::npos,
         "with none found it exits 127 and says what to install, got " +
             std::to_string(none.exit_code) + " [" + none.stderr_text + "]");
}

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
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");
  const auto build = [&root](const ScriptedProcessLauncher& launcher) {
    const project::GitRepository repo(root, launcher);
    return project::BuildGitRepositoryStateFromStatus(repo, root, 7,
                                                      1234);
  };

  {
    const project::GitRepository not_a_repo(temp_dir.path() / "plain", clean_launcher_probe());
    const project::GitRepositoryState state = project::BuildGitRepositoryStateFromStatus(
        not_a_repo, temp_dir.path() / "plain", 7, 1234);
    Expect(state.refresh_error.category == project::GitRefreshErrorCategory::NotARepo,
           "a directory with no .git marker is reported as not a repository");
    Expect(!state.repo_available, "and as having no repository available");
  }

  ScriptedProcessLauncher clean;
  clean.standing_response.exit_code = 0;
  const project::GitRepositoryState clean_state = build(clean);
  Expect(clean_state.refresh_error.category == project::GitRefreshErrorCategory::None,
         "a clean status is not an error");
  Expect(!clean_state.stale, "and is not stale");
  Expect(clean_state.generation == 7 && clean_state.refreshed_at_ms == 1234,
         "the caller's generation and clock reading are stamped through");

  const project::GitRepositoryState missing = build(ScriptedProcessLauncher::MissingProgram());
  Expect(missing.refresh_error.category != project::GitRefreshErrorCategory::None,
         "git failing to run is reported as a refresh error");
  Expect(missing.stale, "and leaves the state marked stale");
  Expect(missing.refresh_error.category == project::GitRefreshErrorCategory::GitNotFound,
         "classified as git missing, not an unknown error");
  Expect(missing.refresh_error.detail.find("git could not be run") != std::string::npos,
         "with a sentence to show: '" + missing.refresh_error.detail + "'");

  ScriptedProcessLauncher truncated;
  truncated.standing_response.exit_code = 0;
  truncated.standing_response.stdout_text = "1 .M N... 100644 100644 100644 ";
  truncated.standing_response.stdout_text += "0000000 0000000 changed.txt";
  truncated.standing_response.stdout_text.push_back('\0');
  truncated.standing_response.truncated = true;
  const project::GitRepositoryState partial = build(truncated);
  Expect(partial.stale, "a truncated status is stale, not a complete change list");
  Expect(partial.refresh_error.category != project::GitRefreshErrorCategory::None,
         "and says why, rather than showing a prefix as the whole truth");
}

void TestStatusServiceRunsThroughTheLauncherItIsGiven() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");

  ScriptedProcessLauncher launcher;
  launcher.standing_response.exit_code = 0;
  Expect(project::GitStageAll(root, launcher), "the staging call reports the scripted success");
  Expect(!launcher.runs.empty(),
         "the service ran git through the launcher it was given, not a local one it "
         "reached for itself");
  Expect(launcher.runs.front().front() == "git", "and the program it ran was git");

  ScriptedProcessLauncher failing = ScriptedProcessLauncher::MissingProgram();
  Expect(!project::GitStageAll(root, failing), "a launcher that cannot run git fails the call");
  Expect(!failing.runs.empty(), "having actually been asked");
}

void TestCompareServiceRunsThroughTheLauncherItIsGiven() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");
  const std::filesystem::path file = root / "a.txt";

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

void TestWriteSideRunsThroughTheLauncherItIsGiven() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");

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
}

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
  while (std::chrono::steady_clock::now() < deadline) {
    service.ApplyReadyResults();
    if (!service.Snapshot(request).loading) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  service.Stop();
  Expect(!launcher.runs.empty(),
         "a blame ran its git through the launcher on its request, not a held local one");
}

void TestLanguageServerStartsThroughTheProjectsLauncher() {
  ScriptedProcessLauncher launcher;
  {
    workspace::LspManager manager;
    manager.RegisterServer({"scripted"}, launcher, {"true"}, "file:///tmp", {},
                           true);
  }
  Expect(!launcher.resolved_argvs.empty() && launcher.resolved_argvs.front().front() == "true",
         "the language server's argv was resolved through the launcher it was registered "
         "with, not a local one the client reached for");

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

}

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
  AddTest(tests, "ProcessLauncher/FirstAvailableCommandRunsTheFirstThatExists",
          TestFirstAvailableCommandRunsTheFirstThatExists);
  AddTest(tests, "ProcessLauncher/WorkspaceFolderExpandsToTheHostRoot",
          TestWorkspaceFolderExpandsToTheHostRoot);
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

}
