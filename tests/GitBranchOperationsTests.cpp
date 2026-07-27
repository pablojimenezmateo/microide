#include "TestSupport.h"

#include "project/GitBranchOperations.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::ClassifyGitOperationFailure;
using microide::project::CreateGitBranch;
using microide::project::GitOperationOutcome;
using microide::project::GitRemoteOperationKind;
using microide::project::ListGitBranches;
using microide::project::PopGitStash;
using microide::project::RunGitRemoteOperation;
using microide::project::StashGitChanges;
using microide::project::SwitchGitBranch;

bool ListContains(const std::vector<std::string>& values, std::string_view name) {
  return std::find(values.begin(), values.end(), name) != values.end();
}

// The classifier is the whole reason these operations are usable: it turns git's
// prose into an outcome the UI can act on. Pin every branch against the actual
// phrases git emits, so a message the UI relies on cannot silently fall back to
// UnknownError.
void TestGitOperationFailureClassification() {
  Expect(ClassifyGitOperationFailure(0, "") == GitOperationOutcome::Success,
         "a clean exit should classify as success");
  Expect(ClassifyGitOperationFailure(0, "Already up to date.\n") ==
             GitOperationOutcome::NothingToDo,
         "pull's no-op message should classify as nothing-to-do");
  Expect(ClassifyGitOperationFailure(0, "Everything up-to-date\n") ==
             GitOperationOutcome::NothingToDo,
         "push's no-op message should classify as nothing-to-do");

  Expect(ClassifyGitOperationFailure(128, "fatal: Authentication failed for 'https://x/'") ==
             GitOperationOutcome::AuthFailed,
         "an authentication failure should classify as auth");
  Expect(ClassifyGitOperationFailure(
             128, "fatal: could not read Username for 'https://x': terminal prompts disabled") ==
             GitOperationOutcome::AuthFailed,
         "a suppressed credential prompt should classify as auth, not unknown");

  Expect(ClassifyGitOperationFailure(
             128, "fatal: The current branch topic has no upstream branch.") ==
             GitOperationOutcome::NoUpstream,
         "a missing upstream should classify as no-upstream");
  Expect(ClassifyGitOperationFailure(
             1, "fatal: 'origin' does not appear to be a git repository") ==
             GitOperationOutcome::NoRemote,
         "a missing remote should classify as no-remote");
  Expect(ClassifyGitOperationFailure(
             1, "! [rejected] main -> main (non-fast-forward)\nhint: Updates were rejected") ==
             GitOperationOutcome::NonFastForward,
         "a rejected push should classify as non-fast-forward");
  Expect(ClassifyGitOperationFailure(
             1, "error: Your local changes to the following files would be overwritten") ==
             GitOperationOutcome::DirtyWorktree,
         "a blocked checkout should classify as dirty-worktree");
  Expect(ClassifyGitOperationFailure(1, "CONFLICT (content): Merge conflict in a.txt") ==
             GitOperationOutcome::Conflict,
         "a merge conflict should classify as conflict");
  Expect(ClassifyGitOperationFailure(
             128, "fatal: Unable to create '/r/.git/index.lock': File exists.") ==
             GitOperationOutcome::RepoLocked,
         "a held index lock should classify as repo-locked");
  Expect(ClassifyGitOperationFailure(128, "fatal: A branch named 'topic' already exists.") ==
             GitOperationOutcome::BadRef,
         "an existing branch name should classify as bad-ref");
  Expect(ClassifyGitOperationFailure(128, "fatal: could not resolve host: github.com") ==
             GitOperationOutcome::NetworkFailed,
         "an unresolvable host should classify as network");
  Expect(ClassifyGitOperationFailure(1, "something entirely unexpected") ==
             GitOperationOutcome::UnknownError,
         "an unrecognized failure should stay unknown rather than be mislabeled");
}

void TestGitBranchListingAndSwitch() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "a.txt", "one\n");
  InitializeGitRepo(root);
  CommitAll(root, "initial", "branch ops fixture");

  const auto initial = ListGitBranches(root);
  Expect(initial.valid, "listing branches in a real repo should succeed");
  Expect(!initial.current.empty(), "a repo on a branch should report a current branch");
  Expect(ListContains(initial.local, initial.current),
         "the current branch should appear in the local list");

  const std::string base_branch = initial.current;
  const auto created = CreateGitBranch(root, "feature/topic");
  Expect(created.success(), "creating a branch should succeed");

  const auto after_create = ListGitBranches(root);
  Expect(after_create.current == "feature/topic",
         "creating a branch with switch -c should check it out");
  Expect(ListContains(after_create.local, "feature/topic"),
         "the new branch should appear in the local list");
  Expect(after_create.remote.empty(), "a repo with no remotes should list no remote branches");

  Expect(CreateGitBranch(root, "feature/topic").outcome == GitOperationOutcome::BadRef,
         "recreating an existing branch should report bad-ref, not a generic failure");

  const auto switched = SwitchGitBranch(root, base_branch);
  Expect(switched.success(), "switching back to the base branch should succeed");
  Expect(ListGitBranches(root).current == base_branch, "the switch should take effect");

  Expect(SwitchGitBranch(root, "does-not-exist").outcome == GitOperationOutcome::BadRef,
         "switching to a missing branch should report bad-ref");
  Expect(SwitchGitBranch(root, "").outcome == GitOperationOutcome::BadRef,
         "an empty branch name should be rejected without spawning git");
}

// Uncommitted work must block a switch rather than being silently carried or lost.
void TestGitSwitchRefusesToOverwriteLocalChanges() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "a.txt", "one\n");
  InitializeGitRepo(root);
  CommitAll(root, "initial", "switch guard fixture");

  const std::string base_branch = ListGitBranches(root).current;
  Expect(CreateGitBranch(root, "other").success(), "creating the second branch should succeed");
  WriteFile(root / "a.txt", "two\n");
  CommitAll(root, "diverge on other", "switch guard fixture");

  Expect(SwitchGitBranch(root, base_branch).success(), "returning to the base branch should work");
  WriteFile(root / "a.txt", "local edit\n");

  const auto blocked = SwitchGitBranch(root, "other");
  Expect(!blocked.success(), "a switch that would clobber local edits must fail");
  Expect(blocked.outcome == GitOperationOutcome::DirtyWorktree,
         "the blocked switch should be reported as a dirty worktree, not an unknown error");
  Expect(ReadFile(root / "a.txt") == "local edit\n", "the local edit must survive the refusal");
}

void TestGitStashRoundTrip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "a.txt", "one\n");
  InitializeGitRepo(root);
  CommitAll(root, "initial", "stash fixture");

  Expect(PopGitStash(root).outcome == GitOperationOutcome::NothingToDo,
         "popping an empty stash should report nothing-to-do, not a failure");

  const auto clean_stash = StashGitChanges(root, "", false);
  Expect(clean_stash.outcome == GitOperationOutcome::NothingToDo,
         "stashing a clean tree should report nothing-to-do");

  WriteFile(root / "a.txt", "stashed edit\n");
  const auto stashed = StashGitChanges(root, "microide test stash", false);
  Expect(stashed.success(), "stashing a dirty tree should succeed");
  Expect(ReadFile(root / "a.txt") == "one\n", "stashing should restore the committed content");

  const auto popped = PopGitStash(root);
  Expect(popped.success(), "popping the stash should succeed");
  Expect(ReadFile(root / "a.txt") == "stashed edit\n", "popping should restore the edit");
}

// Without a remote there is nothing to talk to; every network verb must say so
// precisely instead of hanging on a credential prompt or reporting UnknownError.
void TestGitRemoteOperationsWithoutRemoteAreClassified() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "a.txt", "one\n");
  InitializeGitRepo(root);
  CommitAll(root, "initial", "remote fixture");

  const auto push = RunGitRemoteOperation(root, GitRemoteOperationKind::Push);
  Expect(!push.success(), "pushing with no remote should fail");
  Expect(push.outcome == GitOperationOutcome::NoRemote ||
             push.outcome == GitOperationOutcome::NoUpstream,
         "pushing with no remote should be reported as a missing remote/upstream");

  const auto pull = RunGitRemoteOperation(root, GitRemoteOperationKind::Pull);
  Expect(!pull.success(), "pulling with no remote should fail");
  Expect(pull.outcome == GitOperationOutcome::NoRemote ||
             pull.outcome == GitOperationOutcome::NoUpstream,
         "pulling with no remote should be reported as a missing remote/upstream");
}

// A path that is not a repository must be rejected before any subprocess runs.
void TestGitOperationsOutsideRepositoryAreRejected() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "plain";
  WriteFile(root / "a.txt", "one\n");

  Expect(!ListGitBranches(root).valid, "a non-repository should not produce a branch listing");
  Expect(SwitchGitBranch(root, "main").outcome == GitOperationOutcome::NotARepo,
         "switching outside a repository should report not-a-repo");
  Expect(StashGitChanges(root, "", false).outcome == GitOperationOutcome::NotARepo,
         "stashing outside a repository should report not-a-repo");
  Expect(SwitchGitBranch({}, "main").outcome == GitOperationOutcome::NotARepo,
         "an empty root should report not-a-repo");
}

}  // namespace

void RegisterGitBranchOperationsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitBranchOperations/FailureClassification",
          TestGitOperationFailureClassification);
  AddTest(tests, "GitBranchOperations/BranchListingAndSwitch", TestGitBranchListingAndSwitch);
  AddTest(tests, "GitBranchOperations/SwitchRefusesToOverwriteLocalChanges",
          TestGitSwitchRefusesToOverwriteLocalChanges);
  AddTest(tests, "GitBranchOperations/StashRoundTrip", TestGitStashRoundTrip);
  AddTest(tests, "GitBranchOperations/RemoteOperationsWithoutRemote",
          TestGitRemoteOperationsWithoutRemoteAreClassified);
  AddTest(tests, "GitBranchOperations/OutsideRepositoryRejected",
          TestGitOperationsOutsideRepositoryAreRejected);
}

}  // namespace microide::tests
