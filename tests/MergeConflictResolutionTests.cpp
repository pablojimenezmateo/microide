#include "GitMergeConflictFixtures.h"
#include "TestSupport.h"

#include "compare/MergeConflictKind.h"
#include "compare/MergeModel.h"
#include "workspace/MergeResultValidation.h"

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
      .incoming_content = "incoming added\n",
      .current_content = "current added\n",
  });
  Expect(metadata.kind == MergeFileConflictKind::BothAdded,
         "add/add content should classify as both added");
}

void TestBinaryClassification() {
  std::string binary(8, '\0');
  const auto metadata = ClassifyMergeFileConflict(MergeConflictClassificationInput{
      .incoming_content = binary,
      .current_content = binary,
  });
  Expect(metadata.kind == MergeFileConflictKind::Binary,
         "nul bytes should classify as binary");
  Expect(!metadata.text_hunks_available, "binary conflicts should disable text hunks");
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

}  // namespace

void RegisterMergeConflictResolutionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MergeConflict/BothModifiedClassification", TestBothModifiedClassification);
  AddTest(tests, "MergeConflict/BothAddedClassification", TestBothAddedClassification);
  AddTest(tests, "MergeConflict/BinaryClassification", TestBinaryClassification);
  AddTest(tests, "MergeConflict/BothMergeOrders", TestBothMergeOrders);
  AddTest(tests, "MergeConflict/ValidationBlocksConflictMarkers",
          TestValidationBlocksConflictMarkers);
  AddTest(tests, "MergeConflict/RemainingConflictCount", TestRemainingConflictCount);
  AddTest(tests, "MergeConflict/ConflictMarkerDetection", TestConflictMarkerDetection);
  AddTest(tests, "MergeConflict/CrlfHeavyClassification", TestCrlfHeavyClassification);
}

}  // namespace microide::tests
