#include "TestSupport.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_set>

#include "project/CommitWorkflowChecks.h"
#include "util/GitConflictMarkers.h"
#include "project/GitCommitExecutor.h"
#include "project/GitPorcelainV2Parser.h"
#include "project/GitRepository.h"
#include "project/GitRepositoryState.h"
#include "project/ProjectBackgroundExecutor.h"
#include "workspace/git/CommitWorkflowService.h"
#include "workspace/git/GitRepositoryService.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"

namespace microide::tests {
namespace {

using microide::project::CommitOperationKind;
using microide::project::CommitOperationResultCategory;
using microide::project::CommitPreCheckKind;
using microide::project::CommitPreCheckSeverity;
using microide::project::BuildCommitStagedSummary;
using microide::project::CommitPreChecksAllowExecution;
using microide::project::GitConflictKind;
using microide::project::GitHeadKind;
using microide::project::GitRepositoryEntryKind;
using microide::project::GitRepositoryState;
using microide::project::RunCommitPreChecks;
using microide::workspace::DecodeProjectConfigRecord;
using microide::workspace::EncodeProjectConfigRecord;
using microide::workspace::PersistedCommitDraftState;
using microide::workspace::PersistedProjectConfigState;

const platform::ProcessLauncher& Local() { return platform::LocalProcessLauncher(); }

GitRepositoryState MakeRepositoryState() {
  GitRepositoryState state{
      .repository_root = "/repo",
      .repo_available = true,
      .branch =
          {
              .head_kind = GitHeadKind::Normal,
              .head_oid = "abc123",
              .branch_name = "main",
              .upstream = "origin/main",
              .ahead = 0,
              .behind = 2,
          },
  };
  state.entries.push_back({
      .kind = GitRepositoryEntryKind::Ordinary,
      .path = {.relative_path = "tracked.cpp"},
      .staged = true,
  });
  state.entries.push_back({
      .kind = GitRepositoryEntryKind::Ordinary,
      .path = {.relative_path = "tracked.cpp"},
      .staged = false,
  });
  state.entries.push_back({
      .kind = GitRepositoryEntryKind::Untracked,
      .path = {.relative_path = "new.cpp"},
      .staged = false,
  });
  return state;
}

void TestEmptySubjectBlocksCommit() {
  const GitRepositoryState state = MakeRepositoryState();
  const auto checks = RunCommitPreChecks(state, Local(), "", "", {});
  bool saw_empty_subject = false;
  for (const auto& check : checks) {
    if (check.kind == CommitPreCheckKind::EmptySubject &&
        check.severity == CommitPreCheckSeverity::Blocking) {
      saw_empty_subject = true;
    }
  }
  Expect(saw_empty_subject, "empty subject should be blocking");
  Expect(!CommitPreChecksAllowExecution(checks, {}), "blocking checks should prevent commit");
}

void TestCommitSubjectLengthCountsCharactersNotBytes() {
  const GitRepositoryState state = MakeRepositoryState();
  const microide::project::CommitStagedSummary summary{.file_count = 1};
  const auto has_long_subject = [&](const std::vector<microide::project::CommitPreCheck>& checks) {
    for (const auto& check : checks) {
      if (check.kind == microide::project::CommitPreCheckKind::LongSubject) return true;
    }
    return false;
  };

  std::string cjk;
  for (int i = 0; i < 40; ++i) cjk += "\xe5\xad\x97";
  Expect(!has_long_subject(RunCommitPreChecks(state, Local(), cjk, "", {}, &summary)),
         "a 40-character (120-byte) subject must not be flagged as too long");

  const std::string too_long(80, 'a');
  const auto checks = RunCommitPreChecks(state, Local(), too_long, "", {}, &summary);
  Expect(has_long_subject(checks), "an 80-character subject must still be flagged as too long");
  for (const auto& check : checks) {
    if (check.kind == microide::project::CommitPreCheckKind::LongSubject) {
      Expect(check.severity == microide::project::CommitPreCheckSeverity::Warning,
             "a long subject warns rather than blocks");
      Expect(!microide::project::CommitPreChecksAllowExecution(checks, {}),
             "unacknowledged, the warning still holds the commit");
      std::unordered_set<std::string> acknowledged;
      for (const auto& warning : checks) acknowledged.insert(warning.id);
      Expect(microide::project::CommitPreChecksAllowExecution(checks, acknowledged),
             "acknowledged (with the fixture's other warnings), the commit proceeds");
    }
  }
}

void TestWarningsRequireAcknowledgement() {
  const GitRepositoryState state = MakeRepositoryState();
  const auto checks = RunCommitPreChecks(state, Local(), "subject", "", {});
  Expect(!CommitPreChecksAllowExecution(checks, {}), "warnings should block until acknowledged");
  std::unordered_set<std::string> acknowledged;
  for (const auto& check : checks) {
    if (check.severity == CommitPreCheckSeverity::Warning) {
      acknowledged.insert(check.id);
    }
  }
  Expect(CommitPreChecksAllowExecution(checks, acknowledged),
         "acknowledged warnings should allow commit");
}

void TestCommitDraftPersistenceRoundTrip() {
  PersistedProjectConfigState config{
      .project_base_color = std::nullopt,
      .settings = {},
      .sidebar_policies = {},
      .commit_draft =
          PersistedCommitDraftState{
              .head_oid = "abc123",
              .branch_name = "main",
              .subject = "Fix tests",
              .body = "Body text",
          },
      .branch_review = {},
  };
  std::vector<std::byte> encoded;
  Expect(EncodeProjectConfigRecord(config, &encoded), "encode project config with commit draft");
  PersistedProjectConfigState decoded;
  Expect(DecodeProjectConfigRecord(encoded, &decoded), "decode project config with commit draft");
  Expect(decoded.commit_draft.has_value(), "decoded commit draft should be present");
  Expect(decoded.commit_draft->subject == "Fix tests", "commit draft subject mismatch");
  Expect(decoded.commit_draft->body == "Body text", "commit draft body mismatch");
}

void TestPartialStageWarnsForV2SingleModifiedEntry() {
  GitRepositoryState state{
      .repository_root = "/repo",
      .repo_available = true,
      .branch = {.head_kind = GitHeadKind::Normal, .head_oid = "abc", .branch_name = "main"},
  };
  state.entries.push_back({
      .kind = GitRepositoryEntryKind::Ordinary,
      .status = microide::project::GitFileStatus::Modified,
      .path = {.relative_path = "mm.cpp"},
      .staged = true,
      .worktree_dirty = true,
  });

  const auto checks = RunCommitPreChecks(state, Local(), "subject", "", {});
  bool saw_partial = false;
  for (const auto& check : checks) {
    if (check.kind == CommitPreCheckKind::UnstagedLeftovers) {
      saw_partial = true;
    }
  }
  Expect(saw_partial,
         "a single staged+worktree-dirty (MM) entry must raise the partial-stage warning");

  GitRepositoryState fully_staged = state;
  fully_staged.entries[0].worktree_dirty = false;
  const auto clean_checks = RunCommitPreChecks(fully_staged, Local(), "subject", "", {});
  bool saw_clean = false;
  for (const auto& check : clean_checks) {
    if (check.kind == CommitPreCheckKind::UnstagedLeftovers) {
      saw_clean = true;
    }
  }
  Expect(!saw_clean, "a fully-staged (M.) file must not raise the partial-stage warning");
}

void TestRunCommitPreChecksPrecomputedSummaryMatchesRecompute() {
  GitRepositoryState state{
      .repository_root = "/repo",
      .repo_available = true,
      .branch = {.head_kind = GitHeadKind::Normal, .head_oid = "abc", .branch_name = "main"},
  };
  state.entries.push_back({
      .kind = GitRepositoryEntryKind::Ordinary,
      .status = microide::project::GitFileStatus::Modified,
      .path = {.relative_path = "mm.cpp"},
      .staged = true,
      .worktree_dirty = true,
  });

  const auto summary = BuildCommitStagedSummary(state, Local());
  const auto recomputed = RunCommitPreChecks(state, Local(), "subject", "", {});
  const auto reused = RunCommitPreChecks(state, Local(), "subject", "", {}, &summary);

  Expect(reused.size() == recomputed.size(),
         "precomputed-summary path must yield the same number of checks");
  for (std::size_t i = 0; i < reused.size() && i < recomputed.size(); ++i) {
    Expect(reused[i].kind == recomputed[i].kind && reused[i].severity == recomputed[i].severity &&
               reused[i].message == recomputed[i].message,
           "precomputed-summary path must yield identical checks");
  }
  const bool reused_has_partial = std::any_of(
      reused.begin(), reused.end(), [](const microide::project::CommitPreCheck& check) {
        return check.kind == microide::project::CommitPreCheckKind::UnstagedLeftovers;
      });
  Expect(reused_has_partial,
         "the reference-viewed precomputed summary must still raise the partial-stage warning");
}

void TestStagedDiffConflictMarkerDetection() {
  using microide::util::StagedDiffIntroducesConflictMarker;

  const std::string_view banner_diff =
      "diff --git a/mod.py b/mod.py\n"
      "--- a/mod.py\n"
      "+++ b/mod.py\n"
      "@@ -1,2 +1,3 @@\n"
      " # ============================\n"
      "+VALUE = 1\n"
      " # section\n";
  Expect(!StagedDiffIntroducesConflictMarker(banner_diff),
         "a staged `=======` divider must not be flagged as a conflict marker");

  const std::string_view leaked_diff =
      "diff --git a/mod.cpp b/mod.cpp\n"
      "--- a/mod.cpp\n"
      "+++ b/mod.cpp\n"
      "@@ -1,1 +1,4 @@\n"
      "+<<<<<<< HEAD\n"
      "+ours\n"
      "+=======\n"
      "+>>>>>>> theirs\n";
  Expect(StagedDiffIntroducesConflictMarker(leaked_diff),
         "an added `<<<<<<<` marker must be flagged");

  const std::string_view context_marker_diff =
      "@@ -1,3 +1,3 @@\n"
      " <<<<<<< HEAD\n"
      "-old\n"
      "+new\n";
  Expect(!StagedDiffIntroducesConflictMarker(context_marker_diff),
         "a marker on a context line must not block the commit");

  Expect(!StagedDiffIntroducesConflictMarker("+++ b/<<<<<<<weird\n"),
         "the +++ file header must not be treated as a marker line");
}

void TestClassifyHookFailure() {
  Expect(project::ClassifyCommitFailure(1, "pre-commit hook failed") ==
             CommitOperationResultCategory::HookFailed,
         "hook stderr should classify as hook failure");
  Expect(project::ClassifyCommitFailure(1, "Please tell me who you are") ==
             CommitOperationResultCategory::AuthFailed,
         "missing author should classify as auth failure");
  Expect(project::ClassifyCommitFailure(1, "error: pathspec 'hooks/setup' did not match") ==
             CommitOperationResultCategory::UnknownError,
         "a path containing 'hook' is not a hook failure");
  Expect(project::ClassifyCommitFailure(1, "fatal: couldn't create commit on branch fix-hook-order") ==
             CommitOperationResultCategory::UnknownError,
         "a branch name containing 'hook' is not a hook failure");
}

void TestExecuteCommitInTempRepo() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path& root = temp_dir.path();
  InitializeGitRepo(root);
  WriteFile(root / "file.txt", "hello\n");
  RequireGitCommandSuccess(root, {"add", "file.txt"}, "stage file for commit workflow test");
  const auto result =
      project::ExecuteGitCommit(root, Local(), "Initial commit", "", CommitOperationKind::Create);
  Expect(result.category == CommitOperationResultCategory::Success, "commit should succeed");
}

void TestExecuteCommitPreservesShellSignificantAndLargeBody() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path& root = temp_dir.path();
  InitializeGitRepo(root);
  WriteFile(root / "file.txt", "hello\n");
  RequireGitCommandSuccess(root, {"add", "file.txt"}, "stage file for large-body commit test");

  const std::string subject = "Fix $HOME `backtick` \"quotes\" and 'apostrophes'";
  const std::string long_tail(300000, 'x');
  const std::string body = "Body with $(dangerous) substitution and a long tail:\n" + long_tail;
  const auto result =
      project::ExecuteGitCommit(root, Local(), subject, body, CommitOperationKind::Create);
  Expect(result.category == CommitOperationResultCategory::Success,
         "a huge shell-significant body must commit via -F - stdin");

  microide::project::GitRepository repo(root, Local());
  const auto logged = repo.Execute({"log", "-1", "--format=%B"});
  Expect(logged.success(), "git log should read back the message");
  Expect(logged.output.find(subject) != std::string::npos,
         "the subject must be recorded verbatim");
  Expect(logged.output.find(long_tail) != std::string::npos,
         "the long body must be recorded verbatim (argv limits bypassed)");
}

void TestGitRepositorySummaryAgreesWithFullState() {
  using microide::project::GitRepositoryState;
  using microide::project::ProjectBackgroundExecutor;
  using microide::workspace::GitRepositoryService;
  using microide::workspace::GitSidebarRefreshScope;
  using microide::workspace::OutgoingBaseChoice;

  TemporaryDirectory temp_dir;
  const std::filesystem::path repo = temp_dir.path() / "repo";
  InitializeGitRepo(repo);
  WriteFile(repo / "shared.txt", "base\n");
  CommitAll(repo, "base", "base");
  RequireGitCommandSuccess(repo, {"checkout", "-b", "theirs"}, "branch for conflict");
  WriteFile(repo / "shared.txt", "theirs\n");
  CommitAll(repo, "theirs", "theirs");
  RequireGitCommandSuccess(repo, {"checkout", "main"}, "back to the base branch");
  WriteFile(repo / "shared.txt", "ours\n");
  CommitAll(repo, "ours", "ours");
  RunGitCommand(repo, {"merge", "theirs"});

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);

  const auto check = [&](const char* stage) {
    const GitRepositoryState state = git_service.CurrentState();
    const GitRepositoryService::Summary summary = git_service.CurrentSummary();
    std::size_t conflicted = 0;
    for (const auto& entry : state.entries) {
      if (entry.conflicted) ++conflicted;
    }
    Expect(summary.generation == state.generation,
           std::string(stage) + ": summary generation matches the full state");
    Expect(summary.repo_available == state.repo_available,
           std::string(stage) + ": summary repo_available matches the full state");
    Expect(summary.stale == state.stale, std::string(stage) + ": summary stale matches");
    Expect(summary.conflicted_entry_count == conflicted,
           std::string(stage) + ": summary conflicted count matches the full state (" +
               std::to_string(summary.conflicted_entry_count) + " vs " +
               std::to_string(conflicted) + ")");
    return conflicted;
  };

  check("before any refresh");
  git_service.RunRefreshSynchronouslyForTesting(repo, Local(),
                                                GitSidebarRefreshScope::Full,
                                                OutgoingBaseChoice{}, false);
  Expect(git_service.CurrentState().repo_available, "fixture repo should be available");
  Expect(check("after the conflicted refresh") > 0,
         "the fixture must actually leave a conflicted entry, else the count proves nothing");

  const std::uint64_t first_generation = git_service.CurrentState().generation;
  RequireGitCommandSuccess(repo, {"checkout", "--ours", "shared.txt"}, "resolve the conflict");
  RequireGitCommandSuccess(repo, {"add", "shared.txt"}, "stage the resolution");
  git_service.RunRefreshSynchronouslyForTesting(repo, Local(),
                                                GitSidebarRefreshScope::Full,
                                                OutgoingBaseChoice{}, false);
  Expect(git_service.CurrentState().generation != first_generation,
         "the second refresh must install its own state, not leave the first one behind");
  Expect(check("after resolving") == 0, "the resolved tree reports no conflicted entries");
}

void TestCommitResultIsMarshaledToMainThread() {
  using microide::project::ProjectBackgroundExecutor;
  using microide::workspace::CommitWorkflowService;
  using microide::workspace::CommitWorkflowState;
  using microide::workspace::GitRepositoryService;
  using microide::workspace::GitSidebarRefreshScope;
  using microide::workspace::OutgoingBaseChoice;

  TemporaryDirectory temp_dir;
  const std::filesystem::path repo = temp_dir.path() / "repo";
  InitializeGitRepo(repo);
  WriteFile(repo / "seed.txt", "seed\n");
  CommitAll(repo, "base", "base");
  WriteFile(repo / "seed.txt", "seed\nmore\n");
  RequireGitCommandSuccess(repo, {"add", "seed.txt"}, "stage change for commit marshaling test");

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);
  git_service.RunRefreshSynchronouslyForTesting(repo, Local(),
                                                GitSidebarRefreshScope::Full,
                                                OutgoingBaseChoice{}, false);
  Expect(git_service.CurrentState().repo_available, "fixture repo should be available");

  CommitWorkflowService service(executor, git_service);
  CommitWorkflowState state;
  service.Open(state);
  state.subject.SetText("Add more");
  for (const auto& check : state.checks) {
    if (check.severity == project::CommitPreCheckSeverity::Warning) {
      state.acknowledged_warning_ids.insert(check.id);
    }
  }

  const bool dispatched = service.RequestCommit(state, CommitOperationKind::Create);
  Expect(dispatched, "a staged change with a subject should dispatch a commit");
  Expect(state.operation_in_flight, "the commit should be in flight immediately after dispatch");

  for (int i = 0; i < 2000 && service.PendingCompletionCount() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Expect(service.PendingCompletionCount() > 0,
         "the worker should queue a completion for the main thread");

  Expect(state.operation_in_flight,
         "the commit result must not be published from the worker thread");
  Expect(state.status_message == "Committing\xE2\x80\xA6",
         "state must stay pending until the main thread drains the completion");

  service.DrainCompletions();
  Expect(!state.operation_in_flight,
         "draining completions on the main thread must publish the commit result");
}

void TestCommitCompletionSurvivesStateDestruction() {
  using microide::project::ProjectBackgroundExecutor;
  using microide::workspace::CommitWorkflowService;
  using microide::workspace::CommitWorkflowState;
  using microide::workspace::GitRepositoryService;
  using microide::workspace::GitSidebarRefreshScope;
  using microide::workspace::OutgoingBaseChoice;

  TemporaryDirectory temp_dir;
  const std::filesystem::path repo = temp_dir.path() / "repo";
  InitializeGitRepo(repo);
  WriteFile(repo / "seed.txt", "seed\n");
  CommitAll(repo, "base", "base");
  WriteFile(repo / "seed.txt", "seed\nmore\n");
  RequireGitCommandSuccess(repo, {"add", "seed.txt"}, "stage change for teardown test");

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);
  git_service.RunRefreshSynchronouslyForTesting(repo, Local(),
                                                GitSidebarRefreshScope::Full,
                                                OutgoingBaseChoice{}, false);
  Expect(git_service.CurrentState().repo_available, "fixture repo should be available");

  CommitWorkflowService service(executor, git_service);
  auto state = std::make_unique<CommitWorkflowState>();
  service.Open(*state);
  state->subject.SetText("Add more");
  for (const auto& check : state->checks) {
    if (check.severity == project::CommitPreCheckSeverity::Warning) {
      state->acknowledged_warning_ids.insert(check.id);
    }
  }
  Expect(service.RequestCommit(*state, CommitOperationKind::Create),
         "a staged change with a subject should dispatch a commit");

  for (int i = 0; i < 2000 && service.PendingCompletionCount() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Expect(service.PendingCompletionCount() > 0,
         "the worker should queue a completion for the main thread");

  state.reset();

  service.DrainCompletions();
  Expect(service.PendingCompletionCount() == 0, "the abandoned completion should be consumed");
}

void TestPublishedCommitDoesNotCancelALaterOne() {
  using microide::project::ProjectBackgroundExecutor;
  using microide::workspace::CommitWorkflowService;
  using microide::workspace::CommitWorkflowState;
  using microide::workspace::GitRepositoryService;
  using microide::workspace::GitSidebarRefreshScope;
  using microide::workspace::OutgoingBaseChoice;

  TemporaryDirectory temp_dir;
  const std::filesystem::path repo = temp_dir.path() / "repo";
  InitializeGitRepo(repo);
  WriteFile(repo / "seed.txt", "seed\n");
  CommitAll(repo, "base", "base");
  WriteFile(repo / "seed.txt", "seed\nfirst\n");
  RequireGitCommandSuccess(repo, {"add", "seed.txt"}, "stage first change");

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);
  git_service.RunRefreshSynchronouslyForTesting(repo, Local(),
                                                GitSidebarRefreshScope::Full,
                                                OutgoingBaseChoice{}, false);

  CommitWorkflowService service(executor, git_service);

  const auto acknowledge_and_commit = [&](CommitWorkflowState& state, const char* subject) {
    service.Open(state);
    state.subject.SetText(subject);
    for (const auto& check : state.checks) {
      if (check.severity == project::CommitPreCheckSeverity::Warning) {
        state.acknowledged_warning_ids.insert(check.id);
      }
    }
    return service.RequestCommit(state, CommitOperationKind::Create);
  };

  auto first = std::make_unique<CommitWorkflowState>();
  Expect(acknowledge_and_commit(*first, "First"), "first commit should dispatch");
  for (int i = 0; i < 2000 && service.PendingCompletionCount() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  service.DrainCompletions();
  Expect(!first->operation_in_flight, "the first commit should have published");

  WriteFile(repo / "seed.txt", "seed\nfirst\nsecond\n");
  RequireGitCommandSuccess(repo, {"add", "seed.txt"}, "stage second change");
  git_service.RunRefreshSynchronouslyForTesting(repo, Local(),
                                                GitSidebarRefreshScope::Full,
                                                OutgoingBaseChoice{}, false);
  CommitWorkflowState second;
  Expect(acknowledge_and_commit(second, "Second"), "second commit should dispatch");
  first.reset();

  for (int i = 0; i < 2000 && service.PendingCompletionCount() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  service.DrainCompletions();
  Expect(!second.operation_in_flight,
         "closing an already-published state must not cancel the live commit");
}

void TestConflictMarkerScanGate() {
  using microide::workspace::GitRepositoryService;
  using microide::workspace::GitSidebarRefreshScope;
  using microide::workspace::OutgoingBaseChoice;
  using microide::project::ProjectBackgroundExecutor;

  TemporaryDirectory temp_dir;
  const std::filesystem::path& repo = temp_dir.path();
  InitializeGitRepo(repo);
  WriteFile(repo / "seed.txt", "seed\n");
  CommitAll(repo, "base", "base");
  WriteFile(repo / "conflict.txt", "<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> other\n");
  RequireGitCommandSuccess(repo, {"add", "conflict.txt"}, "stage leaked conflict markers");

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);
  git_service.RunRefreshSynchronouslyForTesting(repo, Local(),
                                                GitSidebarRefreshScope::Full,
                                                OutgoingBaseChoice{}, false);
  const GitRepositoryState state = git_service.CurrentState();
  Expect(state.repo_available, "fixture repo should be available");
  const auto summary = BuildCommitStagedSummary(state, Local());

  auto has_conflict_marker_check = [](const std::vector<microide::project::CommitPreCheck>& checks) {
    for (const auto& check : checks) {
      if (check.kind == CommitPreCheckKind::ConflictMarkers &&
          check.severity == CommitPreCheckSeverity::Blocking) {
        return true;
      }
    }
    return false;
  };

  const auto scanned =
      RunCommitPreChecks(state, Local(), "subject", "", {}, &summary, true);
  Expect(has_conflict_marker_check(scanned),
         "scan=true must surface the blocking conflict-marker check for staged markers");

  const auto skipped =
      RunCommitPreChecks(state, Local(), "subject", "", {}, &summary, false);
  Expect(!has_conflict_marker_check(skipped),
         "scan=false (interactive refresh) must skip the unbounded conflict-marker scan");
}

void TestCommitBodyTextCachesUntilContentEdit() {
  using microide::workspace::CommitWorkflowState;

  CommitWorkflowState state;
  state.body.LoadContent("first line\nsecond line");

  const std::string& first = state.BodyText();
  Expect(first == "first line\nsecond line", "BodyText must serialize the loaded body");

  const std::string& again = state.BodyText();
  Expect(&again == &first, "unchanged body must return the cached string, not rebuild it");
  Expect(again == "first line\nsecond line", "cached body content must stay correct");

  state.body.LoadContent("replaced body");
  const std::string& after_edit = state.BodyText();
  Expect(after_edit == "replaced body",
         "an edit must invalidate the cache and re-serialize the new body");
}

void TestCommitBodyHintIsNotTheMessage() {
  using microide::project::ProjectBackgroundExecutor;
  using microide::workspace::CommitWorkflowService;
  using microide::workspace::CommitWorkflowState;
  using microide::workspace::GitRepositoryService;

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);
  CommitWorkflowService service(executor, git_service);
  CommitWorkflowState state;
  service.Open(state);
  Expect(state.BodyText().empty(), "an untouched body is no message, not '" + state.BodyText() + "'");
  Expect(microide::workspace::MakePersistedCommitDraft(state).body.empty(),
         "and the saved draft has no body either");
}

void TestUnacknowledgedWarningsConfirmRatherThanBlock() {
  using microide::project::ProjectBackgroundExecutor;
  using microide::workspace::CommitWorkflowPendingConfirmation;
  using microide::workspace::CommitWorkflowService;
  using microide::workspace::CommitWorkflowState;
  using microide::workspace::GitRepositoryService;
  using microide::workspace::GitSidebarRefreshScope;
  using microide::workspace::OutgoingBaseChoice;

  TemporaryDirectory temp_dir;
  const auto repo = temp_dir.path() / "repo";
  WriteFile(repo / "seed.txt", "seed\n");
  InitializeGitRepo(repo);
  CommitAll(repo, "base", "base");

  WriteFile(repo / "seed.txt", "seed\nmore\n");
  RequireGitCommandSuccess(repo, {"add", "seed.txt"}, "stage a change to commit");
  WriteFile(repo / "scratch.txt", "not staged, not tracked\n");

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);
  git_service.RunRefreshSynchronouslyForTesting(repo, Local(),
                                                GitSidebarRefreshScope::Full,
                                                OutgoingBaseChoice{}, false);
  Expect(git_service.CurrentState().repo_available, "fixture repo should be available");

  CommitWorkflowService service(executor, git_service);
  std::string confirmation_detail;
  int confirmations = 0;
  service.SetCallbacks(CommitWorkflowService::Callbacks{
      .open_commit_warning_confirmation =
          [&](std::string warnings) {
            ++confirmations;
            confirmation_detail = std::move(warnings);
          },
  });

  CommitWorkflowState state;
  service.Open(state);
  state.subject.SetText("Add more");

  const bool has_untracked_warning =
      std::any_of(state.checks.begin(), state.checks.end(), [](const auto& check) {
        return check.kind == CommitPreCheckKind::UntrackedFiles &&
               check.severity == CommitPreCheckSeverity::Warning;
      });
  Expect(has_untracked_warning, "an untracked file should raise the UntrackedFiles warning");
  Expect(state.acknowledged_warning_ids.empty(),
         "a freshly opened workflow acknowledges nothing");

  Expect(service.RequestCommit(state, CommitOperationKind::Create),
         "a warning must be surfaced as a confirmation, not a silent refusal");
  Expect(!state.operation_in_flight, "no commit may dispatch before the warning is confirmed");
  Expect(confirmations == 1, "exactly one confirmation should be requested");
  Expect(!confirmation_detail.empty(), "the confirmation must say what it is warning about");
  Expect(state.pending_confirmation ==
             CommitWorkflowPendingConfirmation::Warnings,
         "the workflow should be parked on a Warnings confirmation");

  Expect(service.ConfirmPendingOperation(state),
         "confirming the warning should dispatch the commit");
  Expect(state.operation_in_flight, "the commit should be in flight after confirmation");

  for (int i = 0; i < 2000 && service.PendingCompletionCount() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Expect(service.PendingCompletionCount() > 0, "the confirmed commit should actually run");
  service.DrainCompletions();
  Expect(confirmations == 1, "the confirmed commit must not re-ask about the same warnings");
}

void TestCancelledWarningConfirmationCommitsNothing() {
  using microide::project::ProjectBackgroundExecutor;
  using microide::workspace::CommitWorkflowPendingConfirmation;
  using microide::workspace::CommitWorkflowService;
  using microide::workspace::CommitWorkflowState;
  using microide::workspace::GitRepositoryService;
  using microide::workspace::GitSidebarRefreshScope;
  using microide::workspace::OutgoingBaseChoice;

  TemporaryDirectory temp_dir;
  const auto repo = temp_dir.path() / "repo";
  WriteFile(repo / "seed.txt", "seed\n");
  InitializeGitRepo(repo);
  CommitAll(repo, "base", "base");
  WriteFile(repo / "seed.txt", "seed\nmore\n");
  RequireGitCommandSuccess(repo, {"add", "seed.txt"}, "stage a change to commit");
  WriteFile(repo / "scratch.txt", "untracked\n");

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);
  git_service.RunRefreshSynchronouslyForTesting(repo, Local(),
                                                GitSidebarRefreshScope::Full,
                                                OutgoingBaseChoice{}, false);

  CommitWorkflowService service(executor, git_service);
  service.SetCallbacks(CommitWorkflowService::Callbacks{
      .open_commit_warning_confirmation = [](std::string) {},
  });
  CommitWorkflowState state;
  service.Open(state);
  state.subject.SetText("Add more");

  Expect(service.RequestCommit(state, CommitOperationKind::Create),
         "the warning confirmation should open");
  service.CancelPendingConfirmation(state);
  Expect(state.pending_confirmation ==
             CommitWorkflowPendingConfirmation::None,
         "cancelling clears the pending confirmation");
  Expect(!state.operation_in_flight, "cancelling must not commit");
  Expect(state.acknowledged_warning_ids.empty(),
         "cancelling must not acknowledge anything, so the next attempt asks again");
}

void TestCommitOnAStaleSnapshotWaitsForTheRefresh() {
  using microide::project::ProjectBackgroundExecutor;
  using microide::workspace::CommitWorkflowPendingConfirmation;
  using microide::workspace::CommitWorkflowService;
  using microide::workspace::CommitWorkflowState;
  using microide::workspace::GitRepositoryService;
  using microide::workspace::GitSidebarRefreshScope;
  using microide::workspace::OutgoingBaseChoice;

  TemporaryDirectory temp_dir;
  const auto repo = temp_dir.path() / "repo";
  WriteFile(repo / "seed.txt", "seed\n");
  InitializeGitRepo(repo);
  CommitAll(repo, "base", "base");
  WriteFile(repo / "seed.txt", "seed\nmore\n");
  RequireGitCommandSuccess(repo, {"add", "seed.txt"}, "stage a change to commit");

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);
  const auto refresh = [&] {
    git_service.RunRefreshSynchronouslyForTesting(repo, Local(), GitSidebarRefreshScope::Full,
                                                  OutgoingBaseChoice{}, false);
  };
  refresh();

  CommitWorkflowService service(executor, git_service);
  int confirmations = 0;
  int refresh_requests = 0;
  service.SetCallbacks(CommitWorkflowService::Callbacks{
      .request_git_refresh = [&] { ++refresh_requests; },
      .open_commit_warning_confirmation = [&](std::string) { ++confirmations; },
  });
  CommitWorkflowState state;
  service.Open(state);
  state.subject.SetText("Add more");

  WriteFile(repo / "scratch.txt", "untracked\n");
  git_service.MarkStale();

  Expect(service.RequestCommit(state, CommitOperationKind::Create),
         "a commit on a stale snapshot is accepted, to be evaluated after the refresh");
  Expect(!state.operation_in_flight, "nothing may be committed on a stale snapshot");
  Expect(confirmations == 0, "and nothing asked yet: the snapshot does not know the file");
  Expect(refresh_requests == 1, "the request asks for the refresh it waits on");

  refresh();
  service.NoteGitRefreshOutcome(state, git_service.CurrentSummary().generation, "");
  Expect(confirmations == 1,
         "the refreshed snapshot holds the untracked file, so the commit asks about it");
  Expect(state.pending_confirmation == CommitWorkflowPendingConfirmation::Warnings,
         "parked on the warnings confirmation");
  Expect(!state.operation_in_flight, "still nothing committed before the answer");

  service.CancelPendingConfirmation(state);
  refresh();
  service.NoteGitRefreshOutcome(state, git_service.CurrentSummary().generation, "");
  Expect(confirmations == 1, "a commit waits for one refresh, not every refresh after");
}

}

void RegisterCommitWorkflowTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CommitWorkflow/GitRepositorySummaryAgreesWithFullState",
          TestGitRepositorySummaryAgreesWithFullState);
  AddTest(tests, "CommitWorkflow/ConflictMarkerScanGate", TestConflictMarkerScanGate);
  AddTest(tests, "CommitWorkflow/CompletionSurvivesStateDestruction",
          TestCommitCompletionSurvivesStateDestruction);
  AddTest(tests, "CommitWorkflow/PublishedCommitDoesNotCancelALaterOne",
          TestPublishedCommitDoesNotCancelALaterOne);
  AddTest(tests, "CommitWorkflow/ResultMarshaledToMainThread",
          TestCommitResultIsMarshaledToMainThread);
  AddTest(tests, "CommitWorkflow/EmptySubjectBlocks", TestEmptySubjectBlocksCommit);
  AddTest(tests, "CommitWorkflow/SubjectLengthCountsCharactersNotBytes",
          TestCommitSubjectLengthCountsCharactersNotBytes);
  AddTest(tests, "CommitWorkflow/WarningsRequireAck", TestWarningsRequireAcknowledgement);
  AddTest(tests, "CommitWorkflow/WarningsConfirmRatherThanBlock",
          TestUnacknowledgedWarningsConfirmRatherThanBlock);
  AddTest(tests, "CommitWorkflow/CancelledWarningConfirmationCommitsNothing",
          TestCancelledWarningConfirmationCommitsNothing);
  AddTest(tests, "CommitWorkflow/CommitOnAStaleSnapshotWaitsForTheRefresh",
          TestCommitOnAStaleSnapshotWaitsForTheRefresh);
  AddTest(tests, "CommitWorkflow/DraftPersistenceRoundTrip", TestCommitDraftPersistenceRoundTrip);
  AddTest(tests, "CommitWorkflow/PartialStageWarnsForV2SingleModifiedEntry",
          TestPartialStageWarnsForV2SingleModifiedEntry);
  AddTest(tests, "CommitWorkflow/StagedDiffConflictMarkerDetection",
          TestStagedDiffConflictMarkerDetection);
  AddTest(tests, "CommitWorkflow/ClassifyHookFailure", TestClassifyHookFailure);
  AddTest(tests, "CommitWorkflow/PrecomputedSummaryMatchesRecompute",
          TestRunCommitPreChecksPrecomputedSummaryMatchesRecompute);
  AddTest(tests, "CommitWorkflow/ExecuteCommitInTempRepo", TestExecuteCommitInTempRepo);
  AddTest(tests, "CommitWorkflow/ExecuteCommitPreservesShellSignificantAndLargeBody",
          TestExecuteCommitPreservesShellSignificantAndLargeBody);
  AddTest(tests, "CommitWorkflow/BodyTextCachesUntilContentEdit",
          TestCommitBodyTextCachesUntilContentEdit);
  AddTest(tests, "CommitWorkflow/BodyHintIsNotTheMessage", TestCommitBodyHintIsNotTheMessage);
}

}
