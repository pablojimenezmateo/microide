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
      .path = {.relative_path = std::filesystem::path("tracked.cpp"),
               .display_label = "tracked.cpp"},
      .staged = true,
  });
  state.entries.push_back({
      .kind = GitRepositoryEntryKind::Ordinary,
      .path = {.relative_path = std::filesystem::path("tracked.cpp"),
               .display_label = "tracked.cpp"},
      .staged = false,
  });
  state.entries.push_back({
      .kind = GitRepositoryEntryKind::Untracked,
      .path = {.relative_path = std::filesystem::path("new.cpp"), .display_label = "new.cpp"},
      .staged = false,
  });
  return state;
}

void TestEmptySubjectBlocksCommit() {
  const GitRepositoryState state = MakeRepositoryState();
  const auto checks = RunCommitPreChecks(state, "", "", {});
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

// Regression: the subject-length gate counts Unicode scalar values, not bytes.
// A subject that is well within the character limit but exceeds it in bytes
// (accented / CJK text) must NOT be flagged as too long.
void TestCommitSubjectLengthCountsCharactersNotBytes() {
  const GitRepositoryState state = MakeRepositoryState();
  const microide::project::CommitStagedSummary summary{.file_count = 1};
  const auto has_long_subject = [&](const std::vector<microide::project::CommitPreCheck>& checks) {
    for (const auto& check : checks) {
      if (check.kind == microide::project::CommitPreCheckKind::LongSubject) return true;
    }
    return false;
  };

  // 40 three-byte CJK characters = 120 bytes but only 40 characters — under the
  // 72-character limit, so no long-subject check should fire.
  std::string cjk;
  for (int i = 0; i < 40; ++i) cjk += "\xe5\xad\x97";  // 字
  Expect(!has_long_subject(RunCommitPreChecks(state, cjk, "", {}, &summary)),
         "a 40-character (120-byte) subject must not be flagged as too long");

  // 80 characters really is over the limit and must still block.
  const std::string too_long(80, 'a');
  Expect(has_long_subject(RunCommitPreChecks(state, too_long, "", {}, &summary)),
         "an 80-character subject must still be flagged as too long");
}

void TestWarningsRequireAcknowledgement() {
  const GitRepositoryState state = MakeRepositoryState();
  const auto checks = RunCommitPreChecks(state, "subject", "", {});
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

// Regression: git status --porcelain=v2 emits exactly ONE record per path, so a
// partially-staged file (`1 MM` — staged edit plus a further unstaged edit) is a single
// GitRepositoryEntry that is both staged and worktree_dirty. The old two-entries-per-path
// assumption never fired under v2, silently dropping the "commit includes only staged
// hunks" warning.
void TestPartialStageWarnsForV2SingleModifiedEntry() {
  GitRepositoryState state{
      .repository_root = "/repo",
      .repo_available = true,
      .branch = {.head_kind = GitHeadKind::Normal, .head_oid = "abc", .branch_name = "main"},
  };
  state.entries.push_back({
      .kind = GitRepositoryEntryKind::Ordinary,
      .status = microide::project::GitFileStatus::Modified,
      .path = {.relative_path = std::filesystem::path("mm.cpp"), .display_label = "mm.cpp"},
      .staged = true,
      .worktree_dirty = true,
  });

  const auto checks = RunCommitPreChecks(state, "subject", "", {});
  bool saw_partial = false;
  for (const auto& check : checks) {
    if (check.kind == CommitPreCheckKind::UnstagedLeftovers) {
      saw_partial = true;
    }
  }
  Expect(saw_partial,
         "a single staged+worktree-dirty (MM) entry must raise the partial-stage warning");

  // A fully-staged file (M.) — staged but not worktree-dirty — must NOT warn.
  GitRepositoryState fully_staged = state;
  fully_staged.entries[0].worktree_dirty = false;
  const auto clean_checks = RunCommitPreChecks(fully_staged, "subject", "", {});
  bool saw_clean = false;
  for (const auto& check : clean_checks) {
    if (check.kind == CommitPreCheckKind::UnstagedLeftovers) {
      saw_clean = true;
    }
  }
  Expect(!saw_clean, "a fully-staged (M.) file must not raise the partial-stage warning");
}

// Regression: RunCommitPreChecks accepts a precomputed staged summary so the
// commit-workflow refresh does not run the identical `git diff --cached --numstat`
// subprocess twice per refresh on the shell thread. It also holds the caller's
// summary by *reference* (no whole-`files`-vector copy per keystroke); passing the
// summary must still produce byte-identical checks to recomputing it internally,
// and the reference-viewed `files` must still drive the partial-stage warning.
void TestRunCommitPreChecksPrecomputedSummaryMatchesRecompute() {
  GitRepositoryState state{
      .repository_root = "/repo",
      .repo_available = true,
      .branch = {.head_kind = GitHeadKind::Normal, .head_oid = "abc", .branch_name = "main"},
  };
  state.entries.push_back({
      .kind = GitRepositoryEntryKind::Ordinary,
      .status = microide::project::GitFileStatus::Modified,
      .path = {.relative_path = std::filesystem::path("mm.cpp"), .display_label = "mm.cpp"},
      .staged = true,
      .worktree_dirty = true,
  });

  const auto summary = BuildCommitStagedSummary(state);
  const auto recomputed = RunCommitPreChecks(state, "subject", "", {});
  const auto reused = RunCommitPreChecks(state, "subject", "", {}, &summary);

  Expect(reused.size() == recomputed.size(),
         "precomputed-summary path must yield the same number of checks");
  for (std::size_t i = 0; i < reused.size() && i < recomputed.size(); ++i) {
    Expect(reused[i].kind == recomputed[i].kind && reused[i].severity == recomputed[i].severity &&
               reused[i].message == recomputed[i].message,
           "precomputed-summary path must yield identical checks");
  }
  // The partial-stage warning is derived by iterating the precomputed summary's
  // `files`; assert it fires through the reference view (mm.cpp is staged + worktree
  // dirty) so a future "simplify back to a by-value copy" can't silently break the
  // path this test exists to protect.
  const bool reused_has_partial = std::any_of(
      reused.begin(), reused.end(), [](const microide::project::CommitPreCheck& check) {
        return check.kind == microide::project::CommitPreCheckKind::UnstagedLeftovers;
      });
  Expect(reused_has_partial,
         "the reference-viewed precomputed summary must still raise the partial-stage warning");
}

// Regression: a staged `=======` section divider (banner comment, RST/Markdown
// underline) must NOT be mistaken for a leaked conflict marker. Only an *added*
// line beginning with the unambiguous `<<<<<<<` / `>>>>>>>` sigils blocks the
// commit. The old substring predicate matched a bare `=======` anywhere in the
// diff and refused perfectly clean commits.
void TestStagedDiffConflictMarkerDetection() {
  using microide::util::StagedDiffIntroducesConflictMarker;

  // A banner divider of `=` is common, benign content -- must not trip.
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

  // An added `<<<<<<<` sigil at line start is a genuine leaked marker.
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

  // A marker that only appears on a context line (already committed) or a removed
  // line (being resolved) is not introduced by this commit.
  const std::string_view context_marker_diff =
      "@@ -1,3 +1,3 @@\n"
      " <<<<<<< HEAD\n"
      "-old\n"
      "+new\n";
  Expect(!StagedDiffIntroducesConflictMarker(context_marker_diff),
         "a marker on a context line must not block the commit");

  // The `+++ b/path` file header must not be mistaken for an added marker.
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
  // Regression: output that merely contains the word "hook" (in a branch name,
  // path, or message) must NOT be misclassified as a hook failure.
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
      project::ExecuteGitCommit(root, "Initial commit", "", CommitOperationKind::Create);
  Expect(result.category == CommitOperationResultCategory::Success, "commit should succeed");
}

// Regression: the commit message is fed on stdin via `commit -F -`, not `-m`, so
// a body with shell-significant bytes and one far larger than argv limits is
// recorded verbatim rather than mangled or rejected.
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
      project::ExecuteGitCommit(root, subject, body, CommitOperationKind::Create);
  Expect(result.category == CommitOperationResultCategory::Success,
         "a huge shell-significant body must commit via -F - stdin");

  // %B is the raw commit message (subject + blank line + body). It must contain
  // the subject and the long tail verbatim.
  microide::project::GitRepository repo(root);
  const auto logged = repo.Execute({"log", "-1", "--format=%B"});
  Expect(logged.success(), "git log should read back the message");
  Expect(logged.output.find(subject) != std::string::npos,
         "the subject must be recorded verbatim");
  Expect(logged.output.find(long_tail) != std::string::npos,
         "the long body must be recorded verbatim (argv limits bypassed)");
}

// Regression: the background commit result must be published to the shared
// CommitWorkflowState on the MAIN thread, never mutated on the worker thread
// (which would race the render thread reading subject/body/status_message).
// After the worker finishes the git commit, the state must stay "in flight"
// until DrainCompletions() runs on the main thread.
void TestCommitResultIsMarshaledToMainThread() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
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
  // Stage a change so there is something to commit.
  WriteFile(repo / "seed.txt", "seed\nmore\n");
  RequireGitCommandSuccess(repo, {"add", "seed.txt"}, "stage change for commit marshaling test");

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);
  git_service.RunRefreshSynchronouslyForTesting(repo, GitSidebarRefreshScope::Full,
                                                OutgoingBaseChoice{}, false);
  Expect(git_service.CurrentState().repo_available, "fixture repo should be available");

  CommitWorkflowService service(executor, git_service);
  CommitWorkflowState state;
  service.Open(state);
  state.subject.SetText("Add more");
  // Acknowledge any non-blocking warnings so the commit is executable.
  for (const auto& check : state.checks) {
    if (check.severity == project::CommitPreCheckSeverity::Warning) {
      state.acknowledged_warning_ids.insert(check.id);
    }
  }

  const bool dispatched = service.RequestCommit(state, CommitOperationKind::Create);
  Expect(dispatched, "a staged change with a subject should dispatch a commit");
  Expect(state.operation_in_flight, "the commit should be in flight immediately after dispatch");

  // Wait for the worker to finish the git commit and queue its completion for the
  // main thread — WITHOUT cancelling the task (Shutdown/Drain would cancel it if it
  // were still queued). Polling the completion count observes the worker naturally.
  for (int i = 0; i < 2000 && service.PendingCompletionCount() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Expect(service.PendingCompletionCount() > 0,
         "the worker should queue a completion for the main thread");

  // Crucially, the result must NOT have been applied on the worker thread.
  Expect(state.operation_in_flight,
         "the commit result must not be published from the worker thread");
  Expect(state.status_message == "Committing\xE2\x80\xA6",
         "state must stay pending until the main thread drains the completion");

  // Draining on the main thread publishes the result.
  service.DrainCompletions();
  Expect(!state.operation_in_flight,
         "draining completions on the main thread must publish the commit result");
}

// Regression: the ConflictMarkers pre-check runs an unbounded `git diff --cached`
// on the calling thread. Interactive commit-panel refreshes (every keystroke) must
// skip it for speed by passing scan_staged_diff_for_conflict_markers=false; only the
// pre-dispatch check pays for it. Gating off must not surface the blocking check;
// gating on must surface it for the same staged content.
void TestConflictMarkerScanGate() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  using microide::workspace::GitRepositoryService;
  using microide::workspace::GitSidebarRefreshScope;
  using microide::workspace::OutgoingBaseChoice;
  using microide::project::ProjectBackgroundExecutor;

  TemporaryDirectory temp_dir;
  const std::filesystem::path& repo = temp_dir.path();
  InitializeGitRepo(repo);
  WriteFile(repo / "seed.txt", "seed\n");
  CommitAll(repo, "base", "base");
  // Stage a file whose added content leaks a real conflict-marker sigil.
  WriteFile(repo / "conflict.txt", "<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> other\n");
  RequireGitCommandSuccess(repo, {"add", "conflict.txt"}, "stage leaked conflict markers");

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);
  git_service.RunRefreshSynchronouslyForTesting(repo, GitSidebarRefreshScope::Full,
                                                OutgoingBaseChoice{}, false);
  const GitRepositoryState state = git_service.CurrentState();
  Expect(state.repo_available, "fixture repo should be available");
  const auto summary = BuildCommitStagedSummary(state);

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
      RunCommitPreChecks(state, "subject", "", {}, &summary, /*scan=*/true);
  Expect(has_conflict_marker_check(scanned),
         "scan=true must surface the blocking conflict-marker check for staged markers");

  const auto skipped =
      RunCommitPreChecks(state, "subject", "", {}, &summary, /*scan=*/false);
  Expect(!has_conflict_marker_check(skipped),
         "scan=false (interactive refresh) must skip the unbounded conflict-marker scan");
}

// Regression (A-075): CommitWorkflowState::BodyText() memoizes the serialized
// commit body against the body viewport's content_revision, so a precheck +
// persisted-draft build on the same keystroke does not re-snapshot and
// re-concatenate the whole body twice. A matching revision must return the
// cached string (same storage); a content edit must invalidate it.
void TestCommitBodyTextCachesUntilContentEdit() {
  using microide::workspace::CommitWorkflowState;

  CommitWorkflowState state;
  state.body.LoadContent("first line\nsecond line");

  const std::string& first = state.BodyText();
  Expect(first == "first line\nsecond line", "BodyText must serialize the loaded body");

  // No content edit between calls: the cache is reused (same backing storage).
  const std::string& again = state.BodyText();
  Expect(&again == &first, "unchanged body must return the cached string, not rebuild it");
  Expect(again == "first line\nsecond line", "cached body content must stay correct");

  // A content edit bumps content_revision and must invalidate the cache.
  state.body.LoadContent("replaced body");
  const std::string& after_edit = state.BodyText();
  Expect(after_edit == "replaced body",
         "an edit must invalidate the cache and re-serialize the new body");
}

// An untracked file in the repo raises the UntrackedFiles pre-check, whose severity
// is Warning. CommitPreChecksAllowExecution refuses on ANY unacknowledged warning,
// and the only writer of acknowledged_warning_ids (AcknowledgeWarning) had no caller
// anywhere in src/ -- so the set was permanently empty and this commit could never
// be dispatched from the UI at all. Same for a branch behind upstream, or a staged
// file with further unstaged edits: all extremely common, all hard blocks with
// purely informational message text and no way to proceed.
//
// Now a warning raises a confirmation instead of a refusal, and confirming it
// dispatches the operation the user originally asked for.
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
  // The trigger: an untracked, unstaged file. Nothing about it should stop a commit.
  WriteFile(repo / "scratch.txt", "not staged, not tracked\n");

  ProjectBackgroundExecutor executor;
  GitRepositoryService git_service(executor);
  git_service.RunRefreshSynchronouslyForTesting(repo, GitSidebarRefreshScope::Full,
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

  // First request: does NOT commit, asks instead.
  Expect(service.RequestCommit(state, CommitOperationKind::Create),
         "a warning must be surfaced as a confirmation, not a silent refusal");
  Expect(!state.operation_in_flight, "no commit may dispatch before the warning is confirmed");
  Expect(confirmations == 1, "exactly one confirmation should be requested");
  Expect(!confirmation_detail.empty(), "the confirmation must say what it is warning about");
  Expect(state.pending_confirmation ==
             CommitWorkflowPendingConfirmation::Warnings,
         "the workflow should be parked on a Warnings confirmation");

  // Confirming acknowledges the warnings and runs the original operation.
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

// Cancelling must leave the workflow exactly as it was: nothing acknowledged (so the
// next attempt asks again) and nothing committed.
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
  git_service.RunRefreshSynchronouslyForTesting(repo, GitSidebarRefreshScope::Full,
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

}  // namespace

void RegisterCommitWorkflowTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CommitWorkflow/ConflictMarkerScanGate", TestConflictMarkerScanGate);
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
}

}  // namespace microide::tests
