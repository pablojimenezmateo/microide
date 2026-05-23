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
      .path = {.relative_path = std::filesystem::path("tracked.cpp")},
      .staged = true,
  });
  state.entries.push_back({
      .kind = GitRepositoryEntryKind::Ordinary,
      .path = {.relative_path = std::filesystem::path("tracked.cpp")},
      .staged = false,
  });
  state.entries.push_back({
      .kind = GitRepositoryEntryKind::Untracked,
      .path = {.relative_path = std::filesystem::path("new.cpp")},
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
      .commit_draft =
          PersistedCommitDraftState{
              .head_oid = "abc123",
              .branch_name = "main",
              .subject = "Fix tests",
              .body = "Body text",
          },
  };
  std::vector<std::byte> encoded;
  Expect(EncodeProjectConfigRecord(config, &encoded), "encode project config with commit draft");
  PersistedProjectConfigState decoded;
  Expect(DecodeProjectConfigRecord(encoded, &decoded), "decode project config with commit draft");
  Expect(decoded.commit_draft.has_value(), "decoded commit draft should be present");
  Expect(decoded.commit_draft->subject == "Fix tests", "commit draft subject mismatch");
  Expect(decoded.commit_draft->body == "Body text", "commit draft body mismatch");
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
  AddTest(tests, "CommitWorkflow/ClassifyHookFailure", TestClassifyHookFailure);
  AddTest(tests, "CommitWorkflow/ExecuteCommitInTempRepo", TestExecuteCommitInTempRepo);
}

}  // namespace microide::tests
