#include "TestSupport.h"

#include "compare/MergeModel.h"

#include <string_view>

namespace microide::tests {
namespace {

using microide::compare::BuildMergeModel;
using microide::compare::BootstrapMergeResultText;
using microide::compare::MergeChoice;
using microide::compare::MergeChoiceLines;
using microide::compare::MergeResultLines;

void TestMergeSingleSidedChange() {
  auto model = BuildMergeModel("alpha\nbeta\ngamma\n", "alpha\nbeta-incoming\ngamma\n",
                               "alpha\nbeta\ngamma\n");
  Expect(model.hunks.size() == 1, "single-sided merge should produce one hunk");
  Expect(!model.hunks.front().conflict, "single-sided merge should not conflict");
  Expect(model.hunks.front().choice == MergeChoice::Incoming,
         "single-sided merge should bootstrap to the changed side");

  const auto result = MergeResultLines(model);
  Expect(result.size() == 4, "single-sided merge should preserve trailing empty line");
  Expect(result[1] == "beta-incoming", "single-sided merge should use incoming change");
}

void TestMergeIndependentChanges() {
  const auto model = BuildMergeModel("one\ntwo\nthree\nfour\n", "one\ntwo-incoming\nthree\nfour\n",
                                     "one\ntwo\nthree\nfour-current\n");
  Expect(model.hunks.size() == 2, "independent changes should stay as separate hunks");
  Expect(!model.hunks[0].conflict && !model.hunks[1].conflict,
         "independent changes should not conflict");

  const auto result = MergeResultLines(model);
  Expect(result[1] == "two-incoming", "merge should keep incoming-only change");
  Expect(result[3] == "four-current", "merge should keep current-only change");
}

void TestMergeConflictChoiceHandling() {
  auto model = BuildMergeModel("keep\nbase-line\n", "keep\nincoming-line\n",
                               "keep\ncurrent-line\n");
  Expect(model.hunks.size() == 1, "overlapping edits should produce one hunk");
  Expect(model.hunks.front().conflict, "overlapping edits should conflict");

  const auto base_result = MergeResultLines(model);
  Expect(base_result[1] == "base-line", "conflict should default to base content");

  model.hunks.front().choice = MergeChoice::Incoming;
  const auto incoming_result = MergeResultLines(model);
  Expect(incoming_result[1] == "incoming-line", "incoming choice should update merge result");

  model.hunks.front().choice = MergeChoice::Current;
  const auto current_result = MergeResultLines(model);
  Expect(current_result[1] == "current-line", "current choice should update merge result");
}

void TestMergeIdenticalInsertions() {
  const auto model =
      BuildMergeModel("top\nbottom\n", "top\nshared\nbottom\n", "top\nshared\nbottom\n");
  Expect(model.hunks.size() == 1, "matching insertions should still form one hunk");
  Expect(!model.hunks.front().conflict, "matching insertions should auto-resolve");

  const auto result = MergeResultLines(model);
  Expect(result[1] == "shared", "matching insertions should appear once in the merge result");
}

void TestMergeBothChoiceConcatenatesConflictInsertions() {
  auto model = BuildMergeModel("top\nbottom\n", "top\nincoming\nbottom\n",
                               "top\ncurrent\nbottom\n");
  Expect(model.hunks.size() == 1, "conflicting insertions should produce one hunk");
  Expect(model.hunks.front().conflict,
         "different insertions at the same position should conflict");

  model.hunks.front().choice = MergeChoice::Both;
  const auto lines = MergeChoiceLines(model.hunks.front(), model.hunks.front().choice);
  Expect(lines.size() == 2, "both choice should keep both insertion blocks");
  Expect(lines[0] == "incoming", "both choice should keep incoming lines first");
  Expect(lines[1] == "current", "both choice should append current lines");
}

void TestMergeChoiceLabels() {
  Expect(std::string_view(microide::compare::MergeChoiceLabel(MergeChoice::Incoming)) ==
             "incoming",
         "incoming merge choice label should match");
  Expect(std::string_view(microide::compare::MergeChoiceLabel(MergeChoice::Current)) ==
             "current",
         "current merge choice label should match");
  Expect(std::string_view(microide::compare::MergeChoiceLabel(MergeChoice::Base)) == "base",
         "base merge choice label should match");
  Expect(std::string_view(microide::compare::MergeChoiceLabel(MergeChoice::Both)) == "both",
         "both merge choice label should match");
}

void TestBootstrapMergeResultTextUsesOneTimeChoices() {
  const auto model = BuildMergeModel("zero\nsame\nlast\n", "zero\nincoming\nlast\n",
                                     "zero\nsame\ncurrent\n");
  Expect(BootstrapMergeResultText(model) == "zero\nincoming\ncurrent\n",
         "bootstrap merge result should apply only the initial per-hunk choices");
}

}  // namespace

void RegisterMergeModelTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Merge/SingleSidedChange", TestMergeSingleSidedChange);
  AddTest(tests, "Merge/IndependentChanges", TestMergeIndependentChanges);
  AddTest(tests, "Merge/ConflictChoiceHandling", TestMergeConflictChoiceHandling);
  AddTest(tests, "Merge/IdenticalInsertions", TestMergeIdenticalInsertions);
  AddTest(tests, "Merge/BothChoiceConcatenatesConflictInsertions",
          TestMergeBothChoiceConcatenatesConflictInsertions);
  AddTest(tests, "Merge/ChoiceLabels", TestMergeChoiceLabels);
  AddTest(tests, "Merge/BootstrapMergeResultTextUsesOneTimeChoices",
          TestBootstrapMergeResultTextUsesOneTimeChoices);
}

}  // namespace microide::tests
