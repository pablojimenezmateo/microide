#include "GitMergeConflictFixtures.h"
#include "TestSupport.h"

#include "compare/MergeConflictKind.h"
#include "compare/MergeModel.h"
#include "workspace/MergeResultValidation.h"
#include "workspace/WorkspaceCompareInteractionCoordinator.h"

#include <filesystem>
#include <system_error>

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
  AddTest(tests, "MergeConflict/RemainingConflictCount", TestRemainingConflictCount);
  AddTest(tests, "MergeConflict/ConflictMarkerDetection", TestConflictMarkerDetection);
  AddTest(tests, "MergeConflict/CrlfHeavyClassification", TestCrlfHeavyClassification);
}

}  // namespace microide::tests
