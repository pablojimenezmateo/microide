#include "TestSupport.h"

#include "compare/MergeModel.h"
#include "editor/TextLayout.h"
#include "workspace/git/MergeResolverContext.h"
#include "workspace/state/WorkspaceTabState.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::compare::BuildMergeDisplayModel;
using microide::compare::BuildMergeModel;
using microide::compare::BootstrapMergeResultText;
using microide::compare::MergeChoice;
using microide::compare::MergeChoiceLines;
using microide::compare::MergeResultLines;
using microide::compare::MergeResultText;

void TestMergeDisplayModelSkipsZeroRowDeletionHunks() {
  // Both sides delete the same middle line — an auto-resolved deletion whose
  // selected lines are all empty (row_count == 0). The display hunk must not be
  // recorded with an inverted end_row < start_row range.
  auto model = BuildMergeModel("a\nb\nc\n", "a\nc\n", "a\nc\n");
  const auto display = BuildMergeDisplayModel(model);
  for (const auto& hunk : display.hunks) {
    Expect(hunk.end_row >= hunk.start_row,
           "every display hunk must have a non-inverted row range");
    Expect(hunk.start_row >= 0 && hunk.end_row < static_cast<int>(display.rows.size()),
           "display hunk row range stays within the row list");
  }
  // The resolved result still drops the deleted line.
  const auto result = MergeResultLines(model);
  Expect(result.size() == 3, "both-sides deletion resolves to a\\nc\\n plus trailing empty");
  Expect(result[0] == "a" && result[1] == "c", "the shared deletion is applied");
}

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

void TestMergeInsertionAndReplacementAtSameBoundaryStaySeparateHunks() {
  const auto model = BuildMergeModel("alpha\nbeta\ngamma\n", "alpha\ninserted\nbeta\ngamma\n",
                                     "alpha\nbeta-current\ngamma\n");
  Expect(model.hunks.size() == 2,
         "insertion at a replacement boundary should not be merged into the replacement hunk");
  Expect(model.hunks[0].base_start == 1 && model.hunks[0].base_end == 1,
         "boundary insertion should remain a zero-width hunk");
  Expect(model.hunks[1].base_start == 1 && model.hunks[1].base_end == 2,
         "replacement should keep its original base span");
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

void TestMergeResultTextHonorsRequestedLineEnding() {
  auto model = BuildMergeModel("alpha\r\nbeta\r\n", "alpha\r\nbeta-incoming\r\n",
                               "alpha\r\nbeta\r\n");
  model.hunks.front().choice = MergeChoice::Incoming;
  Expect(MergeResultText(model, "\r\n") == "alpha\r\nbeta-incoming\r\n",
         "merge result text should preserve the caller-selected line ending");
}

void TestMergeLargeInputsUseSharedFallbackDiff() {
  std::string base;
  std::string incoming;
  std::string current;
  for (int line = 0; line < 900; ++line) {
    const std::string text = "line " + std::to_string(line) + "\n";
    base += text;
    incoming += line == 450 ? "line 450 incoming\n" : text;
    current += line == 451 ? "line 451 current\n" : text;
  }

  const auto model = BuildMergeModel(base, incoming, current);
  Expect(model.hunks.size() == 2,
         "large merge inputs should still produce independent hunks without exhausting memory");
  const auto result = MergeResultLines(model);
  Expect(result[450] == "line 450 incoming" && result[451] == "line 451 current",
         "large merge inputs should keep both sides' independent edits");
}

}  // namespace

// The hover-preview overlay caches its choice lines on the tab (keyed by
// conflict/choice/model revision) so it no longer reallocates them every frame.
void TestEnsureMergePreviewLinesCachesByKey() {
  using microide::workspace::EnsureMergePreviewLines;
  using microide::workspace::MergeTabState;
  using microide::workspace::MergeTrackedConflict;

  const auto model = BuildMergeModel("keep\nbase\n", "keep\nincoming\n", "keep\ncurrent\n");
  Expect(!model.hunks.empty(), "fixture should produce a conflict hunk");

  MergeTabState tab;
  tab.model = model;
  tab.conflicts.push_back(MergeTrackedConflict{.hunk_index = 0, .valid = true});

  const auto to_vector = [](std::span<const std::string> lines) {
    return std::vector<std::string>(lines.begin(), lines.end());
  };

  const std::span<const std::string> incoming = EnsureMergePreviewLines(
      tab, 0, microide::compare::MergeChoice::Incoming);
  Expect(to_vector(incoming) ==
             MergeChoiceLines(tab.model.hunks[0], microide::compare::MergeChoice::Incoming),
         "preview should equal MergeChoiceLines for the chosen side");
  const std::string* cached_ptr = tab.preview_lines_cache.data();

  EnsureMergePreviewLines(tab, 0, microide::compare::MergeChoice::Incoming);
  Expect(tab.preview_lines_cache.data() == cached_ptr,
         "an identical key must return the cache without rebuilding");

  const std::span<const std::string> current = EnsureMergePreviewLines(
      tab, 0, microide::compare::MergeChoice::Current);
  Expect(to_vector(current) ==
             MergeChoiceLines(tab.model.hunks[0], microide::compare::MergeChoice::Current),
         "changing the choice must rebuild with the new content");

  ++tab.model_revision;
  EnsureMergePreviewLines(tab, 0, microide::compare::MergeChoice::Current);
  Expect(to_vector(tab.preview_lines_cache) ==
             MergeChoiceLines(tab.model.hunks[0], microide::compare::MergeChoice::Current),
         "a model-revision bump must rebuild but stay correct");

  Expect(EnsureMergePreviewLines(tab, 5, microide::compare::MergeChoice::Incoming).empty(),
         "an out-of-range conflict index returns an empty span");

  MergeTabState invalid_tab;
  invalid_tab.model = model;
  invalid_tab.conflicts.push_back(MergeTrackedConflict{.hunk_index = 0, .valid = false});
  Expect(EnsureMergePreviewLines(invalid_tab, 0, microide::compare::MergeChoice::Incoming).empty(),
         "an invalid conflict returns an empty span");
}

// The preview now renders through the tab-aware layout path, so a leading tab
// expands to tab_size columns instead of the single column a codepoint slice gave.
void TestMergePreviewLayoutIsTabAware() {
  const editor::LayoutLine layout =
      editor::TextLayout::BuildVisibleLine("\tabc", /*horizontal_scroll=*/0,
                                           /*visible_columns=*/8, /*tab_size=*/4);
  Expect(layout.visual_columns >= 4 + 3,
         "a leading tab must expand to tab_size visual columns plus the trailing glyphs");
}

void RegisterMergeModelTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Merge/EnsureMergePreviewLinesCachesByKey",
          TestEnsureMergePreviewLinesCachesByKey);
  AddTest(tests, "Merge/PreviewLayoutIsTabAware", TestMergePreviewLayoutIsTabAware);
  AddTest(tests, "Merge/SingleSidedChange", TestMergeSingleSidedChange);
  AddTest(tests, "Merge/IndependentChanges", TestMergeIndependentChanges);
  AddTest(tests, "Merge/ConflictChoiceHandling", TestMergeConflictChoiceHandling);
  AddTest(tests, "Merge/IdenticalInsertions", TestMergeIdenticalInsertions);
  AddTest(tests, "Merge/BothChoiceConcatenatesConflictInsertions",
          TestMergeBothChoiceConcatenatesConflictInsertions);
  AddTest(tests, "Merge/InsertionAndReplacementAtSameBoundaryStaySeparateHunks",
          TestMergeInsertionAndReplacementAtSameBoundaryStaySeparateHunks);
  AddTest(tests, "Merge/ChoiceLabels", TestMergeChoiceLabels);
  AddTest(tests, "Merge/BootstrapMergeResultTextUsesOneTimeChoices",
          TestBootstrapMergeResultTextUsesOneTimeChoices);
  AddTest(tests, "Merge/ResultTextHonorsRequestedLineEnding",
          TestMergeResultTextHonorsRequestedLineEnding);
  AddTest(tests, "Merge/LargeInputsUseSharedFallbackDiff",
          TestMergeLargeInputsUseSharedFallbackDiff);
  AddTest(tests, "Merge/DisplayModelSkipsZeroRowDeletionHunks",
          TestMergeDisplayModelSkipsZeroRowDeletionHunks);
}

}  // namespace microide::tests
