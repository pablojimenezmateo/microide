#include "GitMergeConflictFixtures.h"
#include "TestSupport.h"

#include "compare/MergeConflictKind.h"
#include "compare/MergeModel.h"
#include "util/GitConflictMarkers.h"
#include "workspace/git/MergeResultValidation.h"
#include "workspace/coordinators/WorkspaceCompareInteractionCoordinator.h"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace microide::tests {
namespace {

using microide::compare::ClassifyMergeFileConflict;
using microide::compare::MergeChoice;
using microide::compare::MergeChoiceLines;
using microide::compare::MergeConflictClassificationInput;
using microide::compare::MergeFileConflictKind;
using microide::workspace::CountRemainingMergeConflicts;
using microide::workspace::MergeResultContainsConflictMarkers;
using microide::workspace::MergeTabState;
using microide::workspace::MergeTrackedConflict;
using microide::workspace::MergeValidationIssue;
using microide::workspace::MergeValidationRequest;
using microide::workspace::MergeValidationResult;
using microide::workspace::ValidateMergeResult;
using microide::workspace::CompareInteractionCoordinator;
using microide::workspace::ProjectWorkspaceState;

// Builds a delete-resolution merge tab (existence-choice conflict emptied to signal
// deletion) whose working file exists on disk, plus the minimal Operations wiring the
// delete branch of MarkMergeResolved touches. `stage_ok` decides whether the injected
// git stage succeeds.
struct DeleteResolutionHarness {
  ProjectWorkspaceState state;
  MergeTabState merge_tab;

  CompareInteractionCoordinator::Operations MakeOperations(bool stage_ok) {
    CompareInteractionCoordinator::Operations ops;
    ops.active_merge_tab = [this]() { return &merge_tab; };
    ops.request_editor_surface_redraw = []() {};
    ops.request_tab_strip_redraw = []() {};
    ops.stage_merge_result_path = [stage_ok](const std::filesystem::path&) { return stage_ok; };
    return ops;
  }
};

// A delete-conflict resolution whose git staging fails must NOT lose the working file:
// the removal is rolled back with the original bytes and the tab stays unresolved.
void TestDeleteConflictStageFailureRestoresFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path result_path = temp_dir.path() / "conflicted.txt";
  const std::string original = "original conflicted content\nsecond line\n";
  WriteFile(result_path, original);

  DeleteResolutionHarness harness;
  harness.merge_tab.file_conflict.requires_existence_choice = true;
  harness.merge_tab.file_conflict.text_hunks_available = true;
  harness.merge_tab.result_viewport.LoadContent("", {}, harness.merge_tab.result_line_ending);
  harness.merge_tab.output_path = result_path;

  CompareInteractionCoordinator coordinator(harness.state,
                                            harness.MakeOperations(/*stage_ok=*/false));
  coordinator.MarkMergeResolved();

  Expect(std::filesystem::exists(result_path),
         "a failed stage must roll back the file removal, not lose the working file");
  Expect(ReadFile(result_path) == original,
         "the rolled-back file must hold the original bytes verbatim");
  Expect(!harness.merge_tab.marked_resolved,
         "a failed stage must leave the delete conflict unresolved");
}

// The success path still deletes and stages the file and marks the tab resolved.
void TestDeleteConflictStageSuccessRemovesFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path result_path = temp_dir.path() / "conflicted.txt";
  WriteFile(result_path, "original conflicted content\n");

  DeleteResolutionHarness harness;
  harness.merge_tab.file_conflict.requires_existence_choice = true;
  harness.merge_tab.file_conflict.text_hunks_available = true;
  harness.merge_tab.result_viewport.LoadContent("", {}, harness.merge_tab.result_line_ending);
  harness.merge_tab.output_path = result_path;

  CompareInteractionCoordinator coordinator(harness.state,
                                            harness.MakeOperations(/*stage_ok=*/true));
  coordinator.MarkMergeResolved();

  Expect(!std::filesystem::exists(result_path),
         "a successful delete resolution removes the working file");
  Expect(harness.merge_tab.marked_resolved,
         "a successful delete resolution marks the conflict resolved");
}

void TestBothModifiedClassification() {
  const auto metadata = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_exists = true,
      .incoming_exists = true,
      .current_exists = true,
      .base_content = "base line\nshared\n",
      .incoming_content = "base line\nincoming change\n",
      .current_content = "base line\ncurrent change\n",
  });
  Expect(metadata.kind == MergeFileConflictKind::BothModified,
         "both-modified fixture should classify as both modified");
}

// A conflicted gitlink is not a text merge — there is nothing to three-way — but
// git reports it with the same `u UU` codes as a content conflict, so it used to
// classify as BothModified and the merge surface offered hunks for what is really
// a commit pointer. The porcelain v2 `<sub>` field is the discriminator.
void TestSubmoduleConflictClassification() {
  project::GitRepositoryEntry entry;
  entry.kind = project::GitRepositoryEntryKind::Unmerged;
  entry.conflicted = true;
  entry.conflict_kind = project::GitConflictKind::BothModified;
  entry.submodule = true;

  const auto metadata = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .repository_entry = &entry,
      .base_exists = true,
      .incoming_exists = true,
      .current_exists = true,
      .base_content = "Subproject commit aaa\n",
      .incoming_content = "Subproject commit bbb\n",
      .current_content = "Subproject commit ccc\n",
  });
  Expect(metadata.kind == MergeFileConflictKind::Submodule,
         "a conflicted gitlink must classify as a submodule conflict, not both-modified");
  Expect(!metadata.text_hunks_available,
         "a submodule conflict must not offer text hunks");
}

// git's D/F conflict. git does NOT report the colliding path: it leaves the
// directory in place and moves the file side aside, so the unmerged record names
// `thing~file-side` while `thing` is the directory. The refresh probe sets this
// flag; see GitRepositoryService/RefreshMarksFileDirectoryConflict for the
// end-to-end case against real git.
void TestFileDirectoryConflictClassification() {
  project::GitRepositoryEntry entry;
  entry.kind = project::GitRepositoryEntryKind::Unmerged;
  entry.conflicted = true;
  entry.conflict_kind = project::GitConflictKind::BothAdded;
  entry.path_is_directory = true;

  const auto metadata = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .repository_entry = &entry,
      .base_exists = false,
      .incoming_exists = true,
      .current_exists = true,
      .base_content = {},
      .incoming_content = "file side\n",
      .current_content = {},
  });
  Expect(metadata.kind == MergeFileConflictKind::FileDirectory,
         "a conflicted path that is a directory on disk is a file/directory conflict");
  Expect(!metadata.text_hunks_available,
         "a file/directory conflict must not offer text hunks");

  // A submodule checkout is also a directory; the more specific (and more
  // actionable) submodule message must win.
  entry.submodule = true;
  const auto submodule_wins = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .repository_entry = &entry,
      .base_exists = true,
      .incoming_exists = true,
      .current_exists = true,
      .base_content = "Subproject commit aaa\n",
      .incoming_content = "Subproject commit bbb\n",
      .current_content = "Subproject commit ccc\n",
  });
  Expect(submodule_wins.kind == MergeFileConflictKind::Submodule,
         "a submodule outranks file/directory — its checkout is a directory too");

  // A conflicted path that is an ordinary file is unaffected.
  entry.submodule = false;
  entry.path_is_directory = false;
  const auto ordinary = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .repository_entry = &entry,
      .base_exists = true,
      .incoming_exists = true,
      .current_exists = true,
      .base_content = "base\n",
      .incoming_content = "incoming\n",
      .current_content = "current\n",
  });
  Expect(ordinary.kind != MergeFileConflictKind::FileDirectory,
         "an ordinary conflicted file must not classify as file/directory");
}

void TestBothAddedClassification() {
  const auto metadata = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_exists = false,
      .incoming_exists = true,
      .current_exists = true,
      .base_content = {},
      .incoming_content = "incoming added\n",
      .current_content = "current added\n",
  });
  Expect(metadata.kind == MergeFileConflictKind::BothAdded,
         "add/add content should classify as both added");
}

void TestSingleSideAddWithoutBaseIsNotDelete() {
  // No base means a side that has the file *added* it; the existence-fallback used
  // to mislabel these as deletes (offering a spurious existence choice) because it
  // ignored base_exists.
  const auto us = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_exists = false,
      .incoming_exists = false,
      .current_exists = true,
      .base_content = {},
      .incoming_content = {},
      .current_content = "current added\n",
  });
  Expect(us.kind == MergeFileConflictKind::AddedByUs,
         "no base + only current present should classify as added-by-us");
  Expect(!us.requires_existence_choice, "an add is not a delete: no existence choice");

  const auto them = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_exists = false,
      .incoming_exists = true,
      .current_exists = false,
      .base_content = {},
      .incoming_content = "incoming added\n",
      .current_content = {},
  });
  Expect(them.kind == MergeFileConflictKind::AddedByThem,
         "no base + only incoming present should classify as added-by-them");
  Expect(!them.requires_existence_choice, "an add is not a delete: no existence choice");
}

void TestSingleSideDeleteWithBaseStillClassifiesAsDelete() {
  const auto them = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_exists = true,
      .incoming_exists = false,
      .current_exists = true,
      .base_content = "base\n",
      .incoming_content = {},
      .current_content = "current change\n",
  });
  Expect(them.kind == MergeFileConflictKind::DeletedByThem,
         "base present + incoming missing should stay deleted-by-them");
  Expect(them.requires_existence_choice, "a real delete still needs an existence choice");

  const auto us = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_exists = true,
      .incoming_exists = true,
      .current_exists = false,
      .base_content = "base\n",
      .incoming_content = "incoming change\n",
      .current_content = {},
  });
  Expect(us.kind == MergeFileConflictKind::DeletedByUs,
         "base present + current missing should stay deleted-by-us");
  Expect(us.requires_existence_choice, "a real delete still needs an existence choice");
}

void TestBinaryClassification() {
  std::string binary(8, '\0');
  const auto metadata = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_content = {},
      .incoming_content = binary,
      .current_content = binary,
  });
  Expect(metadata.kind == MergeFileConflictKind::Binary,
         "nul bytes should classify as binary");
  Expect(!metadata.text_hunks_available, "binary conflicts should disable text hunks");
}

void TestBothDeletedClassification() {
  // base present, both sides absent: a real git "DD" conflict. The existence
  // fallback used to omit this combination and leave the kind Unknown, which
  // wrongly offered a text-hunk view and suppressed the keep/delete choice.
  const auto metadata = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_exists = true,
      .incoming_exists = false,
      .current_exists = false,
      .base_content = "base\n",
      .incoming_content = {},
      .current_content = {},
  });
  Expect(metadata.kind == MergeFileConflictKind::BothDeleted,
         "base present + both sides absent should classify as both-deleted");
  Expect(metadata.requires_existence_choice,
         "both-deleted needs the keep/delete existence choice");
  Expect(!metadata.text_hunks_available, "both-deleted has no text hunks to show");
}

void TestBinaryDoesNotClobberExistenceChoice() {
  // A delete/modify conflict whose surviving side is binary must keep its
  // existence-choice kind; the unconditional binary override used to overwrite it
  // with Binary, stripping the keep-vs-delete decision the user needs.
  std::string binary(8, '\0');
  const auto them = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_exists = true,
      .incoming_exists = false,
      .current_exists = true,
      .base_content = "base\n",
      .incoming_content = {},
      .current_content = binary,
  });
  Expect(them.kind == MergeFileConflictKind::DeletedByThem,
         "binary surviving content must not downgrade a delete/modify to Binary");
  Expect(them.requires_existence_choice,
         "the existence choice must survive the binary check");
}

void TestBothMergeOrders() {
  microide::compare::MergeModel model = microide::compare::BuildMergeModel(
      "base\n", "incoming\n", "current\n");
  Expect(!model.hunks.empty(), "merge model should contain a conflict hunk");
  auto& hunk = model.hunks.front();
  const auto current_first = MergeChoiceLines(hunk, MergeChoice::BothCurrentFirst);
  const auto incoming_first = MergeChoiceLines(hunk, MergeChoice::BothIncomingFirst);
  Expect(current_first.size() == 2 && current_first[0] == "current" && current_first[1] == "incoming",
         "both-current-first should place current lines before incoming lines");
  Expect(incoming_first.size() == 2 && incoming_first[0] == "incoming" &&
             incoming_first[1] == "current",
         "both-incoming-first should place incoming lines before current lines");
}

void TestValidationBlocksConflictMarkers() {
  MergeTabState merge_tab;
  merge_tab.result_viewport.LoadContent("before\n<<<<<<< HEAD\nside\n=======\nother\n>>>>>>> x\n",
                                        {}, merge_tab.result_line_ending);
  merge_tab.result_viewport.SetDirty(false);
  const MergeValidationResult validation = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab,
      .project_root = {},
      .result_should_exist = true,
  });
  Expect(!validation.ok && validation.issue == MergeValidationIssue::ConflictMarkers,
         "validation should block conflict markers");
  // The reported marker line is the first `<<<<<<<` line (index 1 here). This exercises
  // the single-pass LineSpan scan that replaced serialize + a second Snapshot()
  // (TD-2026-07-17A-009): the "markers present" verdict and the marker line index now
  // come from one walk of the live buffer.
  Expect(validation.marker_line == std::optional<std::size_t>(1),
         "validation should report the first conflict-marker line");
}

// The single-pass conflict scanner reproduces the old serialize+span behavior:
// `complete` requires all three sigils anywhere; `first_marker_line` is the first
// line ANCHORED at `<<<<<<<`. A partial marker set is not complete, and a `<<<<<<<`
// that is not at line start is not the anchored first line.
void TestScanConflictMarkersMatchesLegacyBehavior() {
  using microide::util::ConflictMarkerScan;
  using microide::util::ScanConflictMarkers;

  const std::vector<std::string> full = {"before", "<<<<<<< HEAD", "side",
                                         "=======", "other", ">>>>>>> x"};
  const ConflictMarkerScan complete = ScanConflictMarkers(full);
  Expect(complete.complete, "all three sigils present should read as complete");
  Expect(complete.first_marker_line == std::optional<std::size_t>(1),
         "first anchored `<<<<<<<` is line 1");

  // Only two of the three sigils: not a complete conflict.
  const std::vector<std::string> partial = {"<<<<<<< HEAD", "side", "======="};
  Expect(!ScanConflictMarkers(partial).complete,
         "a missing `>>>>>>>` sigil should not read as complete");

  // A `<<<<<<<` mid-line contributes to `complete` (substring semantics) but is not
  // the anchored first-marker line.
  const std::vector<std::string> mid_line = {"x <<<<<<< y", "=======", ">>>>>>>"};
  const ConflictMarkerScan mid = ScanConflictMarkers(mid_line);
  Expect(mid.complete, "mid-line sigils still count toward completeness");
  Expect(mid.first_marker_line == std::nullopt,
         "a non-line-anchored `<<<<<<<` is not the first marker line");

  Expect(!ScanConflictMarkers(std::vector<std::string>{}).complete,
         "empty input has no conflict markers");
}

void TestRemainingConflictCount() {
  const std::vector<MergeTrackedConflict> conflicts = {
      {.valid = true, .resolved = false},
      {.valid = true, .resolved = true},
  };
  Expect(CountRemainingMergeConflicts(conflicts) == 1,
         "remaining conflict count should ignore resolved hunks");
}

void TestConflictMarkerDetection() {
  Expect(MergeResultContainsConflictMarkers("<<<<<<< HEAD\na\n=======\nb\n>>>>>>> branch\n"),
         "marker detector should find git conflict markers");
}

void TestCrlfHeavyClassification() {
  const auto metadata = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_content = "line\n",
      .incoming_content = "line\r\n",
      .current_content = "line\r\n",
  });
  Expect(metadata.kind == MergeFileConflictKind::LineEndingHeavy,
         "crlf-only differences should classify as line-ending-heavy");
}

// "Sides differ only by line endings" is the label a user reads before deciding it
// is safe to take one side whole, so it must not appear when a side rewrote real
// content. It used to: one side normalizing away was enough (2026-07-10 pass).
void TestLineEndingHeavyRequiresBothSidesToBeEndingOnly() {
  const auto one_sided = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_exists = true,
      .incoming_exists = true,
      .current_exists = true,
      .base_content = "alpha\nbeta\n",
      // Incoming only rewrote the endings...
      .incoming_content = "alpha\r\nbeta\r\n",
      // ...while current made a genuine content change.
      .current_content = "alpha\nBETA CHANGED\n",
  });
  Expect(one_sided.kind == MergeFileConflictKind::BothModified,
         "a real content change on one side is not a line-ending conflict");

  // Both sides made the SAME content change and disagree only on endings: that is
  // still purely an ending conflict, and the old predicate missed it because it
  // required both raw sides to be non-normalized.
  const auto agreeing_sides = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_exists = true,
      .incoming_exists = true,
      .current_exists = true,
      .base_content = "alpha\nbeta\n",
      .incoming_content = "alpha\nBETA CHANGED\n",
      .current_content = "alpha\r\nBETA CHANGED\r\n",
  });
  Expect(agreeing_sides.kind == MergeFileConflictKind::LineEndingHeavy,
         "sides that agree on content and differ only in endings are a line-ending conflict");
  Expect(agreeing_sides.summary.find("whitespace") == std::string::npos,
         "the summary must not claim whitespace detection the classifier does not do");
}

// Mark Resolved saves the result (bumping the file mtime) and then validates it.
// Regression: the resolver must refresh disk_result_tick to the saved file's mtime
// so its own write is not flagged as an external modification, which previously
// rejected every Mark Resolved and never staged the file.
void TestMarkResolvedRefreshesDiskTick() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path result_path = temp_dir.path() / "resolved.txt";
  WriteFile(result_path, "clean resolved content\nno markers here\n");

  MergeTabState merge_tab;
  merge_tab.result_viewport.LoadContent("clean resolved content\nno markers here\n", {},
                                        merge_tab.result_line_ending);
  merge_tab.result_viewport.SetDirty(false);
  merge_tab.output_path = result_path;
  // Stale open-time tick, as if the resolver's own save had since rewritten the file.
  merge_tab.disk_result_tick = 1;

  const MergeValidationResult stale = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab,
      .project_root = {},
      .result_should_exist = true,
  });
  Expect(!stale.ok && stale.issue == MergeValidationIssue::ExternalModification,
         "a disk tick disagreeing with the file mtime flags external modification");

  // Refresh the tick to the file's current mtime (what Mark Resolved now does).
  merge_tab.disk_result_tick = microide::workspace::FileModificationTick(result_path);
  const MergeValidationResult refreshed = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab,
      .project_root = {},
      .result_should_exist = true,
  });
  Expect(refreshed.ok, "after refreshing the disk tick, validation accepts the saved result");
}

// A modify/delete conflict resolves to deletion when the user reduces the result to
// empty content. ResolvedResultShouldExist must derive this from the serialized
// content, not from result_viewport.lines().empty() (always false), so the delete
// path is reachable and ValidateMergeResult accepts a non-existent file.
void TestDeleteConflictResolvesByDeletion() {
  MergeTabState merge_tab;
  merge_tab.file_conflict.requires_existence_choice = true;

  // Non-empty content: keep the file.
  merge_tab.result_viewport.LoadContent("kept content\n", {}, merge_tab.result_line_ending);
  Expect(microide::workspace::ResolvedResultShouldExist(merge_tab),
         "a non-empty existence-choice result should keep the file");

  // Empty content: accept the deletion. lines() is still non-empty ({\"\"}) here, so
  // this specifically exercises the serialized-content path.
  merge_tab.result_viewport.LoadContent("", {}, merge_tab.result_line_ending);
  Expect(!merge_tab.result_viewport.lines().empty(),
         "an emptied buffer is normalized to one line (why lines().empty() cannot signal delete)");
  Expect(!microide::workspace::ResolvedResultShouldExist(merge_tab),
         "an empty existence-choice result should resolve to deletion");

  // With result_should_exist=false and no file on disk, validation must pass (the
  // delete path removes the file before staging).
  merge_tab.result_viewport.SetDirty(false);
  merge_tab.output_path = std::filesystem::temp_directory_path() / "microide-merge-deleted-xyz.txt";
  std::error_code ec;
  std::filesystem::remove(merge_tab.output_path, ec);
  const MergeValidationResult validation = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab,
      .project_root = {},
      .result_should_exist = false,
  });
  Expect(validation.ok,
         "a delete resolution validates when the file is absent and result_should_exist is false");

  // An ordinary conflict always expects the file to exist.
  MergeTabState ordinary;
  ordinary.result_viewport.LoadContent("", {}, ordinary.result_line_ending);
  Expect(microide::workspace::ResolvedResultShouldExist(ordinary),
         "a non-existence-choice conflict always expects the file to exist");
}

// After the app's own Save, the file-change watcher can flag external_result_stale.
// The tick re-sync must clear that flag, otherwise the boolean guard short-circuits
// before the (now matching) tick comparison and rejects Mark Resolved forever.
void TestExternalStaleClearedAfterSelfSave() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path result_path = temp_dir.path() / "resolved.txt";
  WriteFile(result_path, "clean\n");

  MergeTabState merge_tab;
  merge_tab.result_viewport.LoadContent("clean\n", {}, merge_tab.result_line_ending);
  merge_tab.result_viewport.SetDirty(false);
  merge_tab.output_path = result_path;
  merge_tab.disk_result_tick = microide::workspace::FileModificationTick(result_path);
  merge_tab.external_result_stale = true;  // as the watcher would set after our own save

  const MergeValidationResult stale = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab, .project_root = {}, .result_should_exist = true});
  Expect(!stale.ok && stale.issue == MergeValidationIssue::ExternalModification,
         "the latched external-stale flag blocks Mark Resolved");

  // The save path / MarkMergeResolved tick re-sync clears the flag.
  merge_tab.external_result_stale = false;
  const MergeValidationResult cleared = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab, .project_root = {}, .result_should_exist = true});
  Expect(cleared.ok, "clearing external_result_stale after reconciling the tick unblocks Mark Resolved");
}

}  // namespace

void RegisterMergeConflictResolutionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MergeConflict/FileDirectoryConflictClassification",
          TestFileDirectoryConflictClassification);
  AddTest(tests, "MergeConflict/SubmoduleConflictClassification",
          TestSubmoduleConflictClassification);
  AddTest(tests, "MergeConflict/DeleteConflictResolvesByDeletion",
          TestDeleteConflictResolvesByDeletion);
  AddTest(tests, "MergeConflict/DeleteConflictStageFailureRestoresFile",
          TestDeleteConflictStageFailureRestoresFile);
  AddTest(tests, "MergeConflict/DeleteConflictStageSuccessRemovesFile",
          TestDeleteConflictStageSuccessRemovesFile);
  AddTest(tests, "MergeConflict/ExternalStaleClearedAfterSelfSave",
          TestExternalStaleClearedAfterSelfSave);
  AddTest(tests, "MergeConflict/MarkResolvedRefreshesDiskTick",
          TestMarkResolvedRefreshesDiskTick);
  AddTest(tests, "MergeConflict/BothModifiedClassification", TestBothModifiedClassification);
  AddTest(tests, "MergeConflict/BothAddedClassification", TestBothAddedClassification);
  AddTest(tests, "MergeConflict/SingleSideAddWithoutBaseIsNotDelete",
          TestSingleSideAddWithoutBaseIsNotDelete);
  AddTest(tests, "MergeConflict/SingleSideDeleteWithBaseStillClassifiesAsDelete",
          TestSingleSideDeleteWithBaseStillClassifiesAsDelete);
  AddTest(tests, "MergeConflict/BinaryClassification", TestBinaryClassification);
  AddTest(tests, "MergeConflict/BothDeletedClassification", TestBothDeletedClassification);
  AddTest(tests, "MergeConflict/BinaryDoesNotClobberExistenceChoice",
          TestBinaryDoesNotClobberExistenceChoice);
  AddTest(tests, "MergeConflict/BothMergeOrders", TestBothMergeOrders);
  AddTest(tests, "MergeConflict/ValidationBlocksConflictMarkers",
          TestValidationBlocksConflictMarkers);
  AddTest(tests, "MergeConflict/ScanConflictMarkersMatchesLegacyBehavior",
          TestScanConflictMarkersMatchesLegacyBehavior);
  AddTest(tests, "MergeConflict/RemainingConflictCount", TestRemainingConflictCount);
  AddTest(tests, "MergeConflict/ConflictMarkerDetection", TestConflictMarkerDetection);
  AddTest(tests, "MergeConflict/CrlfHeavyClassification", TestCrlfHeavyClassification);
  AddTest(tests, "MergeConflict/LineEndingHeavyRequiresBothSides",
          TestLineEndingHeavyRequiresBothSidesToBeEndingOnly);
}

}  // namespace microide::tests
