#include "TestSupport.h"

#include "compare/MergeModel.h"
#include "editor/TextLayout.h"
#include "workspace/git/MergeResolverContext.h"
#include "workspace/state/WorkspaceTabState.h"

#include <span>
#include <cstdint>
#include <filesystem>
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
using microide::compare::MergeModel;
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
  // Current first, then incoming: the file's own conflict-marker order and VS
  // Code's "Accept Both Changes". The explicit incoming-first variant is separate.
  Expect(lines[0] == "current", "both choice should keep current lines first");
  Expect(lines[1] == "incoming", "both choice should append incoming lines");
  const auto incoming_first =
      MergeChoiceLines(model.hunks.front(), MergeChoice::BothIncomingFirst);
  Expect(incoming_first.size() == 2 && incoming_first[0] == "incoming" &&
             incoming_first[1] == "current",
         "the explicit incoming-first variant still orders incoming first");
}

// Two changes that touch — no unchanged base line between them — are ONE
// conflict, as `git merge-file` reports them (an insertion at either boundary
// of the other side's replacement, two deletions that meet). This test used to
// pin the opposite: two clean hunks, applied independently, for a case git
// refuses to merge. Checked against git over 500 random three-way merges.
void TestMergeTouchingChangesAreOneConflict() {
  const auto model = BuildMergeModel("alpha\nbeta\ngamma\n", "alpha\ninserted\nbeta\ngamma\n",
                                     "alpha\nbeta-current\ngamma\n");
  Expect(model.hunks.size() == 1,
         "an insertion at a replacement's boundary joins the replacement's hunk");
  Expect(model.hunks[0].conflict, "the joined hunk is a conflict, as in git");
  Expect(model.hunks[0].base_start == 1 && model.hunks[0].base_end == 2,
         "the hunk spans the replaced base line");

  const auto after = BuildMergeModel("alpha\nbeta\ngamma\n", "alpha\nbeta\ninserted\ngamma\n",
                                     "alpha\nbeta-current\ngamma\n");
  Expect(after.hunks.size() == 1 && after.hunks[0].conflict,
         "an insertion right after the replacement is the same conflict");

  const auto apart = BuildMergeModel("alpha\nbeta\nmid\ngamma\n",
                                     "alpha\nbeta\nmid\ninserted\ngamma\n",
                                     "alpha\nbeta-current\nmid\ngamma\n");
  Expect(apart.hunks.size() == 2 && !apart.hunks[0].conflict && !apart.hunks[1].conflict,
         "with an unchanged line between them the two changes merge cleanly");
  Expect(BootstrapMergeResultText(apart) == "alpha\nbeta-current\nmid\ninserted\ngamma\n",
         "and the clean merge is git's");

  // Two sides deleting one of two identical trailing blank lines is one change
  // made twice, not two deletions: the result keeps the other blank line.
  const auto twice = BuildMergeModel("a\n\n\n", "a\n\n", "a\n\n");
  Expect(BootstrapMergeResultText(twice) == "a\n\n", "the same deletion on both sides applies once");
}

// The model's verdicts against `git merge-file` over random three-way edits.
// Every line is unique, so the two diffs cannot legitimately align an edit
// differently (with repeated lines they can, and then the two tools disagree
// on a few merges in a thousand without either being wrong); with that removed
// the agreement is exact: a conflict where git reports one, a clean merge with
// git's text where it does not. This is what caught the touching-changes rule
// (175 of 500 merges git refused were auto-merged) and the phantom alignment.
void TestMergeRandomTriplesAgreeWithGitMergeFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path dir = temp_dir.path();
  std::uint64_t state = 0x2545F4914F6CDD1Dull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  const auto edit = [&](std::vector<std::string> lines, const char* tag) {
    const std::size_t edits = 1 + next(3);
    for (std::size_t e = 0; e < edits; ++e) {
      const std::string fresh = std::string(tag) + std::to_string(e);
      const std::size_t kind = next(3);
      if (kind == 0 && !lines.empty()) {
        lines[next(lines.size())] = fresh;
      } else if (kind == 1 || lines.empty()) {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(next(lines.size() + 1)), fresh);
      } else {
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(next(lines.size())));
      }
    }
    return lines;
  };
  const auto join = [](const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
      text += line;
      text += '\n';
    }
    return text;
  };
  int agreed_clean = 0;
  int agreed_conflict = 0;
  for (int trial = 0; trial < 120; ++trial) {
    std::vector<std::string> base;
    const std::size_t count = 2 + next(9);
    for (std::size_t i = 0; i < count; ++i) {
      base.push_back("b" + std::to_string(i));
    }
    const std::string base_text = join(base);
    const std::string current_text = join(edit(base, "c"));
    const std::string incoming_text = join(edit(base, "i"));
    WriteFile(dir / "base", base_text);
    WriteFile(dir / "current", current_text);
    WriteFile(dir / "incoming", incoming_text);
    const int git_exit = RunCommand("cd '" + dir.string() +
                                    "' && git merge-file -p current base incoming > merged 2>/dev/null");
    const bool git_conflict = git_exit != 0;
    const std::string git_text = ReadFile(dir / "merged");

    const auto model = BuildMergeModel(base_text, incoming_text, current_text);
    bool conflict = false;
    for (const auto& hunk : model.hunks) {
      conflict = conflict || hunk.conflict;
    }
    const std::string label = "trial " + std::to_string(trial) + " base=[" + base_text +
                              "] current=[" + current_text + "] incoming=[" + incoming_text + "]";
    Expect(conflict == git_conflict,
           ("the model conflicts exactly where git merge-file does: " + label).c_str());
    if (!conflict && !git_conflict) {
      Expect(BootstrapMergeResultText(model) == git_text,
             ("a clean merge produces git's text: " + label + " git=[" + git_text + "] model=[" +
              BootstrapMergeResultText(model) + "]")
                 .c_str());
      ++agreed_clean;
    } else if (conflict && git_conflict) {
      ++agreed_conflict;
    }
  }
  Expect(agreed_clean >= 20 && agreed_conflict >= 20,
         "the generator produced both clean and conflicting merges");
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
  // An unchanged line separates the two sides' edits; adjacent edits are one
  // conflict, as in git (see TestMergeTouchingChangesAreOneConflict).
  const auto model = BuildMergeModel("zero\nsame\nmid\nlast\n", "zero\nincoming\nmid\nlast\n",
                                     "zero\nsame\nmid\ncurrent\n");
  Expect(BootstrapMergeResultText(model) == "zero\nincoming\nmid\ncurrent\n",
         "bootstrap merge result should apply only the initial per-hunk choices");
}

// A MergeModel's line vectors — its own and every hunk's — are `string_view`s
// into three shared source buffers rather than owned strings (TD-2026-08-15-246).
// The whole safety argument is that the buffers are `shared_ptr<const string>`,
// so the string OBJECT's address is fixed and a copy or move of the model cannot
// relocate the bytes. That argument is worth a test, because the failure mode is
// a use-after-free that reads plausibly for short lines and only bites on long
// ones (or the other way round, depending on the small-string buffer).
//
// The inputs here are deliberately BOTH shorter and longer than a small-string
// buffer, so a model that had held plain `std::string` members would dangle on at
// least one of them.
void TestMergeModelLinesSurviveCopyAndMove() {
  const std::string short_base = "a\nb\n";
  const std::string long_incoming =
      "a\n" + std::string(200, 'x') + "\nb\n";
  const std::string long_current = "a\n" + std::string(200, 'y') + "\nb\n";

  const auto check = [](const MergeModel& model, std::string_view what) {
    Expect(model.base_lines.size() >= 2, std::string(what) + ": base lines survived");
    Expect(model.base_lines[0] == "a", std::string(what) + ": base line 0 still reads 'a'");
    Expect(model.incoming_lines.size() >= 2, std::string(what) + ": incoming lines survived");
    Expect(model.incoming_lines[1] == std::string(200, 'x'),
           std::string(what) + ": the long incoming line still reads its own bytes");
    Expect(model.current_lines[1] == std::string(200, 'y'),
           std::string(what) + ": the long current line still reads its own bytes");
    for (const auto& hunk : model.hunks) {
      for (std::string_view line : hunk.incoming_lines) {
        Expect(line.find('\0') == std::string_view::npos,
               std::string(what) + ": a hunk's line view must not read freed memory");
      }
    }
  };

  MergeModel built = BuildMergeModel(short_base, long_incoming, long_current);
  check(built, "as built");

  // Move: the source buffers move as shared_ptrs, so the strings themselves do
  // not move at all.
  const MergeModel moved = std::move(built);
  check(moved, "after move");

  // Copy: the copy shares the same buffers, so its views point at the same bytes.
  const MergeModel copied = moved;
  check(copied, "after copy");

  // And the copy outliving its source is the case a plain member would break.
  {
    MergeModel scoped = BuildMergeModel(short_base, long_incoming, long_current);
    const MergeModel survivor = std::move(scoped);
    scoped = MergeModel{};  // drop the source's own references
    check(survivor, "after the source model was reset");
  }

  // The result text must agree with the lines the views report, which is what
  // proves the views are not merely non-crashing but correct.
  Expect(MergeResultText(copied) == MergeResultText(moved),
         "a copied model must produce the same result text as its source");
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
    // Two lines apart: adjacent edits are one conflict, as in git.
    incoming += line == 450 ? "line 450 incoming\n" : text;
    current += line == 452 ? "line 452 current\n" : text;
  }

  const auto model = BuildMergeModel(base, incoming, current);
  Expect(model.hunks.size() == 2,
         "large merge inputs should still produce independent hunks without exhausting memory");
  const auto result = MergeResultLines(model);
  Expect(result[450] == "line 450 incoming" && result[452] == "line 452 current",
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
  AddTest(tests, "Merge/RandomTriplesAgreeWithGitMergeFile",
          TestMergeRandomTriplesAgreeWithGitMergeFile);
  AddTest(tests, "Merge/TouchingChangesAreOneConflict",
          TestMergeTouchingChangesAreOneConflict);
  AddTest(tests, "Merge/ChoiceLabels", TestMergeChoiceLabels);
  AddTest(tests, "Merge/BootstrapMergeResultTextUsesOneTimeChoices",
          TestBootstrapMergeResultTextUsesOneTimeChoices);
  AddTest(tests, "Merge/ModelLinesSurviveCopyAndMove", TestMergeModelLinesSurviveCopyAndMove);
  AddTest(tests, "Merge/ResultTextHonorsRequestedLineEnding",
          TestMergeResultTextHonorsRequestedLineEnding);
  AddTest(tests, "Merge/LargeInputsUseSharedFallbackDiff",
          TestMergeLargeInputsUseSharedFallbackDiff);
  AddTest(tests, "Merge/DisplayModelSkipsZeroRowDeletionHunks",
          TestMergeDisplayModelSkipsZeroRowDeletionHunks);
}

}  // namespace microide::tests
