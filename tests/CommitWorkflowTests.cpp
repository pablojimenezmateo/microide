#include "TestSupport.h"

#include <filesystem>
#include <unordered_set>

#include "project/CommitWorkflowChecks.h"
#include "project/GitCommitExecutor.h"
#include "project/GitPorcelainV2Parser.h"
#include "project/GitRepositoryState.h"
#include "workspace/WorkspacePersistenceFormat.h"

namespace microide::tests {
namespace {

using microide::project::CommitOperationKind;
using microide::project::CommitOperationResultCategory;
using microide::project::CommitPreCheckKind;
using microide::project::CommitPreCheckSeverity;
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

void TestClassifyHookFailure() {
  Expect(project::ClassifyCommitFailure(1, "pre-commit hook failed") ==
             CommitOperationResultCategory::HookFailed,
         "hook stderr should classify as hook failure");
  Expect(project::ClassifyCommitFailure(1, "Please tell me who you are") ==
             CommitOperationResultCategory::AuthFailed,
         "missing author should classify as auth failure");
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

}  // namespace

void RegisterCommitWorkflowTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CommitWorkflow/EmptySubjectBlocks", TestEmptySubjectBlocksCommit);
  AddTest(tests, "CommitWorkflow/WarningsRequireAck", TestWarningsRequireAcknowledgement);
  AddTest(tests, "CommitWorkflow/DraftPersistenceRoundTrip", TestCommitDraftPersistenceRoundTrip);
  AddTest(tests, "CommitWorkflow/PartialStageWarnsForV2SingleModifiedEntry",
          TestPartialStageWarnsForV2SingleModifiedEntry);
  AddTest(tests, "CommitWorkflow/ClassifyHookFailure", TestClassifyHookFailure);
  AddTest(tests, "CommitWorkflow/ExecuteCommitInTempRepo", TestExecuteCommitInTempRepo);
}

}  // namespace microide::tests
