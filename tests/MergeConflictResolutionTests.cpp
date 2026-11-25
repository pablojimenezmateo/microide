#include "GitMergeConflictFixtures.h"
#include "TestSupport.h"

#include "compare/MergeConflictKind.h"
#include "compare/MergeModel.h"
#include "util/GitConflictMarkers.h"
#include "util/TextFileIO.h"
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

struct DeleteResolutionHarness {
  ProjectWorkspaceState state;
  MergeTabState merge_tab;

  CompareInteractionCoordinator::Operations MakeOperations(bool stage_ok) {
    CompareInteractionCoordinator::Operations ops;
    ops.active_merge_tab = [this]() { return &merge_tab; };
    ops.request_frame = []() {};
    ops.request_tab_strip_redraw = []() {};
    ops.stage_merge_result_path = [stage_ok](const std::filesystem::path&) { return stage_ok; };
    return ops;
  }
};

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
                                            harness.MakeOperations(false));
  coordinator.MarkMergeResolved();

  Expect(std::filesystem::exists(result_path),
         "a failed stage must roll back the file removal, not lose the working file");
  Expect(ReadFile(result_path) == original,
         "the rolled-back file must hold the original bytes verbatim");
  Expect(!harness.merge_tab.marked_resolved,
         "a failed stage must leave the delete conflict unresolved");
}

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
                                            harness.MakeOperations(true));
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

void TestMultiCaretEditInTheResultPaneIsValidated() {
  MergeTabState merge_tab;
  merge_tab.result_viewport.LoadContent(
      "before\n<<<<<<< HEAD\nside\n=======\nother\n>>>>>>> x\nafter\n", {},
      merge_tab.result_line_ending);
  merge_tab.result_viewport.SetDirty(false);

  auto& viewport = merge_tab.result_viewport;
  const auto marker_issue = [&]() {
    viewport.SetDirty(false);
    return ValidateMergeResult(MergeValidationRequest{
               .merge_tab = merge_tab,
               .project_root = {},
               .result_should_exist = true,
           })
        .issue;
  };
  Expect(marker_issue() == MergeValidationIssue::ConflictMarkers,
         "the fixture starts with a complete conflict block, or this proves nothing");

  const std::size_t lines_before = viewport.line_count();
  viewport.MoveCursorTo(1, 0);
  viewport.AddSecondaryCaret(3, 0);
  viewport.AddSecondaryCaret(5, 0);
  Expect(viewport.secondary_caret_range_view().size() == 2, "three carets are placed");
  Expect(viewport.DeleteCurrentLine(), "delete-line applies at every caret");

  Expect(viewport.line_count() == lines_before - 3,
         "three marker lines are gone in one step, got " +
             std::to_string(viewport.line_count()) + " from " +
             std::to_string(lines_before));
  Expect(viewport.lines()[0] == "before" && viewport.lines()[1] == "side" &&
             viewport.lines()[2] == "other" && viewport.lines()[3] == "after",
         "exactly the marker lines were removed");
  Expect(marker_issue() != MergeValidationIssue::ConflictMarkers,
         "the scan sees the resolved buffer, not the one it was built for");

  Expect(viewport.Undo(), "the multi-caret delete undoes as one step");
  Expect(viewport.line_count() == lines_before,
         "all three lines come back together, got " + std::to_string(viewport.line_count()) +
             " of " + std::to_string(lines_before));
  Expect(marker_issue() == MergeValidationIssue::ConflictMarkers,
         "the restored conflict blocks again");
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
  Expect(validation.marker_line == std::optional<std::size_t>(1),
         "validation should report the first conflict-marker line");
}

void TestScanConflictMarkersMatchesLegacyBehavior() {
  using microide::util::ConflictMarkerScan;
  using microide::util::ContainsCompleteConflictMarkers;
  using microide::util::ScanConflictMarkers;

  const std::vector<std::string> full = {"before", "<<<<<<< HEAD", "side",
                                         "=======", "other", ">>>>>>> x"};
  const ConflictMarkerScan complete = ScanConflictMarkers(full);
  Expect(complete.complete, "a start, a separator and an end line read as complete");
  Expect(complete.first_marker_line == std::optional<std::size_t>(1),
         "first anchored `<<<<<<<` is line 1");

  const std::vector<std::string> partial = {"<<<<<<< HEAD", "side", "======="};
  Expect(!ScanConflictMarkers(partial).complete,
         "a missing `>>>>>>>` line should not read as complete");

  const std::vector<std::string> quoted = {"// the merge refuses a '<<<<<<<' left behind",
                                           "// ===========================================",
                                           "expect(text.contains(\">>>>>>> branch\"));"};
  const ConflictMarkerScan text = ScanConflictMarkers(quoted);
  Expect(!text.complete, "markers quoted mid-line and a banner comment are not a conflict");
  Expect(text.first_marker_line == std::nullopt,
         "a non-line-anchored `<<<<<<<` is not the first marker line");
  Expect(!ContainsCompleteConflictMarkers("// <<<<<<< ours\n// =======\n// >>>>>>> theirs\n"),
         "the text test agrees: commented-out markers are not a conflict");

  Expect(ContainsCompleteConflictMarkers("<<<<<<< HEAD\r\na\r\n=======\r\nb\r\n>>>>>>> x\r\n"),
         "a CRLF conflict is one");
  Expect(ScanConflictMarkers(std::vector<std::string>{"<<<<<<<<<< HEAD", "a", "==========", "b",
                                                      ">>>>>>>>>> x"})
             .complete,
         "ten-character markers are one too");

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

void TestLineEndingHeavyRequiresBothSidesToBeEndingOnly() {
  const auto one_sided = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .base_exists = true,
      .incoming_exists = true,
      .current_exists = true,
      .base_content = "alpha\nbeta\n",
      .incoming_content = "alpha\r\nbeta\r\n",
      .current_content = "alpha\nBETA CHANGED\n",
  });
  Expect(one_sided.kind == MergeFileConflictKind::BothModified,
         "a real content change on one side is not a line-ending conflict");

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

void TestIdenticalRewriteDoesNotBlockMarkResolved() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path result_path = temp_dir.path() / "resolved.txt";
  const std::string resolved = "clean resolved content\nno markers here\n";
  WriteFile(result_path, resolved);

  MergeTabState merge_tab;
  merge_tab.result_viewport.LoadContent(resolved, {}, merge_tab.result_line_ending);
  merge_tab.result_viewport.SetDirty(false);
  merge_tab.output_path = result_path;
  merge_tab.persisted_output_baseline = resolved;
  merge_tab.disk_result_tick = 1;

  const MergeValidationResult result = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab,
      .project_root = {},
      .result_should_exist = true,
  });
  Expect(result.ok,
         "a moved tick over byte-identical content must not block Mark Resolved");
}

void TestRealExternalEditStillBlocksMarkResolved() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path result_path = temp_dir.path() / "resolved.txt";
  const std::string resolved = "clean resolved content\nno markers here\n";
  WriteFile(result_path, resolved);

  MergeTabState merge_tab;
  merge_tab.result_viewport.LoadContent(resolved, {}, merge_tab.result_line_ending);
  merge_tab.result_viewport.SetDirty(false);
  merge_tab.output_path = result_path;
  merge_tab.persisted_output_baseline = resolved;
  merge_tab.disk_result_tick = 1;

  WriteFile(result_path, "clean resolved content\nedited by something else\n");

  const MergeValidationResult result = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab,
      .project_root = {},
      .result_should_exist = true,
  });
  Expect(!result.ok && result.issue == MergeValidationIssue::ExternalModification,
         "a real external edit still blocks Mark Resolved");
}

void TestMarkResolvedRefreshesDiskTick() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path result_path = temp_dir.path() / "resolved.txt";
  WriteFile(result_path, "clean resolved content\nno markers here\n");

  MergeTabState merge_tab;
  merge_tab.result_viewport.LoadContent("clean resolved content\nno markers here\n", {},
                                        merge_tab.result_line_ending);
  merge_tab.result_viewport.SetDirty(false);
  merge_tab.output_path = result_path;
  merge_tab.disk_result_tick = 1;

  const MergeValidationResult stale = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab,
      .project_root = {},
      .result_should_exist = true,
  });
  Expect(!stale.ok && stale.issue == MergeValidationIssue::ExternalModification,
         "a disk tick disagreeing with the file mtime flags external modification");

  merge_tab.disk_result_tick = microide::util::FileModificationTick(result_path);
  const MergeValidationResult refreshed = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab,
      .project_root = {},
      .result_should_exist = true,
  });
  Expect(refreshed.ok, "after refreshing the disk tick, validation accepts the saved result");
}

void TestDeleteConflictResolvesByDeletion() {
  MergeTabState merge_tab;
  merge_tab.file_conflict.requires_existence_choice = true;

  merge_tab.result_viewport.LoadContent("kept content\n", {}, merge_tab.result_line_ending);
  Expect(microide::workspace::ResolvedResultShouldExist(merge_tab),
         "a non-empty existence-choice result should keep the file");

  merge_tab.result_viewport.LoadContent("", {}, merge_tab.result_line_ending);
  Expect(!merge_tab.result_viewport.lines().empty(),
         "an emptied buffer is normalized to one line (why lines().empty() cannot signal delete)");
  Expect(!microide::workspace::ResolvedResultShouldExist(merge_tab),
         "an empty existence-choice result should resolve to deletion");

  merge_tab.result_viewport.LoadContent("kept content", {}, merge_tab.result_line_ending);
  Expect(merge_tab.result_viewport.lines().size() == 1,
         "the fixture must reach the single-line arm that reads line 0");
  Expect(merge_tab.result_viewport.lines().materialized_line_count() == 0,
         "a freshly loaded buffer has materialized nothing");
  Expect(microide::workspace::ResolvedResultShouldExist(merge_tab),
         "a non-empty one-line existence-choice result keeps the file");
  Expect(merge_tab.result_viewport.lines().materialized_line_count() == 0,
         "the existence check must not materialize a line copy into the result buffer");

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

  MergeTabState ordinary;
  ordinary.result_viewport.LoadContent("", {}, ordinary.result_line_ending);
  Expect(microide::workspace::ResolvedResultShouldExist(ordinary),
         "a non-existence-choice conflict always expects the file to exist");
}

void TestExternalStaleClearedAfterSelfSave() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path result_path = temp_dir.path() / "resolved.txt";
  WriteFile(result_path, "clean\n");

  MergeTabState merge_tab;
  merge_tab.result_viewport.LoadContent("clean\n", {}, merge_tab.result_line_ending);
  merge_tab.result_viewport.SetDirty(false);
  merge_tab.output_path = result_path;
  merge_tab.disk_result_tick = microide::util::FileModificationTick(result_path);
  merge_tab.external_result_stale = true;

  const MergeValidationResult stale = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab, .project_root = {}, .result_should_exist = true});
  Expect(!stale.ok && stale.issue == MergeValidationIssue::ExternalModification,
         "the latched external-stale flag blocks Mark Resolved");

  merge_tab.external_result_stale = false;
  const MergeValidationResult cleared = ValidateMergeResult(MergeValidationRequest{
      .merge_tab = merge_tab, .project_root = {}, .result_should_exist = true});
  Expect(cleared.ok, "clearing external_result_stale after reconciling the tick unblocks Mark Resolved");
}

}

void TestAcceptingASideReportsTheAcceptedLines() {
  const std::string wide(400, 'x');
  const std::string base = "shared\nshort\ntail\n";
  const std::string incoming = "shared\n" + wide + "\ntail\n";
  const std::string current = "shared\ntiny\ntail\n";

  ProjectWorkspaceState state;
  MergeTabState merge_tab;
  merge_tab.model = microide::compare::BuildMergeModel(base, incoming, current);
  merge_tab.file_conflict.text_hunks_available = true;

  std::size_t conflict_hunk = merge_tab.model.hunks.size();
  for (std::size_t i = 0; i < merge_tab.model.hunks.size(); ++i) {
    if (merge_tab.model.hunks[i].conflict) {
      conflict_hunk = i;
      break;
    }
  }
  Expect(conflict_hunk < merge_tab.model.hunks.size(),
         "the fixture should produce a conflicting hunk, or this test proves nothing");

  microide::workspace::MergeTrackedConflict tracked;
  tracked.hunk_index = conflict_hunk;
  tracked.start_line = 1;
  tracked.end_line = 2;
  tracked.valid = true;
  merge_tab.conflicts.push_back(tracked);
  merge_tab.selected_hunk = 0;
  merge_tab.result_viewport.LoadContent(current, {}, merge_tab.result_line_ending);

  std::vector<std::vector<std::string>> reported;
  CompareInteractionCoordinator::Operations ops;
  ops.active_merge_tab = [&]() { return &merge_tab; };
  ops.request_tab_strip_redraw = []() {};
  ops.reveal_active_merge_selection = []() {};
  ops.request_frame = []() {};
  ops.update_merge_max_visual_columns = [&](MergeTabState&,
                                            const std::vector<std::string>& lines) {
    reported.push_back(lines);
  };

  CompareInteractionCoordinator coordinator(state, ops);
  coordinator.ApplyMergeChoice(MergeChoice::Incoming);

  Expect(reported.size() == 1, "accepting a side should report its width exactly once");
  Expect(!reported.front().empty(),
         "the width update must receive the accepted lines, not a moved-from vector");
  bool saw_wide = false;
  for (const std::string& line : reported.front()) {
    saw_wide = saw_wide || line.size() >= wide.size();
  }
  Expect(saw_wide, "the reported lines must include the wide line that was accepted");
}

void TestMergeResultStateFollowsTheResultsContent() {
  using microide::workspace::ComputeMergeResultState;
  using microide::workspace::MergeResultState;
  const std::string conflicted = "one\n<<<<<<< ours\nmine\n=======\ntheirs\n>>>>>>> in\ntwo\n";
  MergeTabState merge_tab;
  merge_tab.file_conflict.text_hunks_available = true;
  merge_tab.result_viewport.LoadContent(conflicted, {}, merge_tab.result_line_ending);
  const auto state = [&] {
    merge_tab.result_viewport.SetDirty(false);
    return ComputeMergeResultState(merge_tab, merge_tab.file_conflict);
  };
  Expect(state() == MergeResultState::Invalid, "a result holding conflict markers is invalid");
  Expect(state() == MergeResultState::Invalid, "and still is when asked again");
  Expect(merge_tab.result_viewport.ReplaceLines(1, 6, {"mine"}), "the conflict is replaced by one side");
  Expect(state() == MergeResultState::Saved, "a result edited free of markers is saved");
  Expect(merge_tab.result_viewport.Undo(), "the edit undoes");
  Expect(state() == MergeResultState::Invalid, "the markers back, the result is invalid again");
  Expect(merge_tab.result_viewport.Redo(), "the edit redoes");
  Expect(state() == MergeResultState::Saved, "and saved again");
  merge_tab.result_viewport.LoadContent(conflicted, {}, merge_tab.result_line_ending);
  Expect(state() == MergeResultState::Invalid, "a reloaded result is scanned afresh");
  merge_tab.result_viewport.LoadContent("one\ntwo\n", {}, merge_tab.result_line_ending);
  Expect(state() == MergeResultState::Saved, "and again when the reload has no markers");
  merge_tab.marked_resolved = true;
  Expect(state() == MergeResultState::Resolved, "a result marked resolved is resolved");
  merge_tab.marked_resolved = false;
  merge_tab.index_stale = true;
  Expect(state() == MergeResultState::Stale, "a stale index outranks the rest");
}

void RegisterMergeConflictResolutionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MergeConflict/IdenticalRewriteDoesNotBlockMarkResolved",
          TestIdenticalRewriteDoesNotBlockMarkResolved);
  AddTest(tests, "MergeConflict/RealExternalEditStillBlocksMarkResolved",
          TestRealExternalEditStillBlocksMarkResolved);
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
  AddTest(tests, "MergeConflict/AcceptingASideReportsTheAcceptedLines",
          TestAcceptingASideReportsTheAcceptedLines);
  AddTest(tests, "MergeConflict/MultiCaretEditInTheResultPaneIsValidated",
          TestMultiCaretEditInTheResultPaneIsValidated);
  AddTest(tests, "MergeConflict/ValidationBlocksConflictMarkers",
          TestValidationBlocksConflictMarkers);
  AddTest(tests, "MergeConflict/ScanConflictMarkersMatchesLegacyBehavior",
          TestScanConflictMarkersMatchesLegacyBehavior);
  AddTest(tests, "MergeConflict/ResultStateFollowsTheResultsContent", TestMergeResultStateFollowsTheResultsContent);
  AddTest(tests, "MergeConflict/RemainingConflictCount", TestRemainingConflictCount);
  AddTest(tests, "MergeConflict/ConflictMarkerDetection", TestConflictMarkerDetection);
  AddTest(tests, "MergeConflict/CrlfHeavyClassification", TestCrlfHeavyClassification);
  AddTest(tests, "MergeConflict/LineEndingHeavyRequiresBothSides",
          TestLineEndingHeavyRequiresBothSidesToBeEndingOnly);
}

}
