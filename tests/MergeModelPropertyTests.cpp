// Randomized property tests for the three-way merge model.
//
// The strong oracle here is the round trip: a merge whose every hunk chooses one
// side must reproduce that side's file byte for byte. The hunks are the only
// places the three files disagree, so choosing "incoming" everywhere IS the
// incoming file -- and anything that drops, duplicates, or misorders a run
// breaks that identity on some input even when the hand-written fixtures pass.
//
// The second property is the pair of decision trees MergeChoiceLineViews,
// MergeChoiceLines and MergeChoiceLineCount form. They were once byte-identical
// copies kept in sync by hand; they must still agree for every choice.

#include "TestSupport.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "compare/MergeModel.h"
#include "util/StringUtil.h"

namespace microide::tests {
namespace {

using microide::compare::MergeChoice;
using microide::compare::MergeDisplayModel;
using microide::compare::MergeHunk;
using microide::compare::MergeModel;

struct Rng {
  std::uint64_t state;
  explicit Rng(std::uint64_t seed) : state(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}
  std::uint64_t Next() {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1Dull;
  }
  std::size_t Below(std::size_t bound) { return bound == 0 ? 0 : Next() % bound; }
  bool Chance(std::size_t percent) { return Below(100) < percent; }
};

const std::vector<std::string>& LinePool() {
  static const std::vector<std::string> pool = {
      "",          " ",       "\t",         "alpha",  "alphabet", "beta",
      "gamma(x)",  "}",       "{",          "delta",  "naïve",    "日本語",
      "emoji 🙂",  "// note", "epsilon(1)", "zeta",
  };
  return pool;
}

std::string Join(const std::vector<std::string>& lines) {
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    out.append(lines[i]);
    if (i + 1 < lines.size()) out.push_back('\n');
  }
  return out;
}

std::vector<std::string> BuildLines(Rng& rng, std::size_t count) {
  std::vector<std::string> lines;
  lines.reserve(count);
  const std::vector<std::string>& pool = LinePool();
  for (std::size_t i = 0; i < count; ++i) lines.push_back(pool[rng.Below(pool.size())]);
  return lines;
}

// Edit `base` the way one branch would: a few scattered deletes, replacements
// and insertions, leaving long shared runs so the aligner has real anchors.
std::vector<std::string> BranchOf(Rng& rng, const std::vector<std::string>& base) {
  const std::vector<std::string>& pool = LinePool();
  std::vector<std::string> out;
  out.reserve(base.size() + 4);
  for (const std::string& line : base) {
    const std::size_t roll = rng.Below(100);
    if (roll < 10) continue;
    if (roll < 22) {
      out.push_back(pool[rng.Below(pool.size())]);
      continue;
    }
    if (roll < 30) out.push_back(pool[rng.Below(pool.size())]);
    out.push_back(line);
  }
  if (rng.Chance(25)) out.push_back(pool[rng.Below(pool.size())]);
  return out;
}

std::string Seed(std::uint64_t seed) { return " [seed=" + std::to_string(seed) + "]"; }

// Choosing one side in every hunk must reproduce that side's file exactly.
void ExpectChoosingOneSideReproducesIt(MergeModel model,
                                       MergeChoice choice,
                                       const std::string& expected,
                                       const char* label,
                                       std::uint64_t seed) {
  for (MergeHunk& hunk : model.hunks) hunk.choice = choice;
  const std::string result = compare::MergeResultText(model);
  Expect(result == expected,
         std::string("choosing ") + label + " everywhere must reproduce that file" + Seed(seed) +
             "\n  got:  " + result + "\n  want: " + expected);
}

void TestMergeChoosingOneSideEverywhereReproducesThatFile() {
  // Guard against the corpus going vacuous: if a future generator change stopped
  // producing three-way disagreement, every assertion below would hold trivially.
  std::size_t total_hunks = 0;
  std::size_t conflict_hunks = 0;
  for (std::uint64_t seed = 1; seed <= 400; ++seed) {
    Rng rng(seed);
    const std::vector<std::string> base_lines = BuildLines(rng, 2 + rng.Below(12));
    const std::vector<std::string> incoming_lines = BranchOf(rng, base_lines);
    const std::vector<std::string> current_lines = BranchOf(rng, base_lines);
    const std::string base = Join(base_lines);
    const std::string incoming = Join(incoming_lines);
    const std::string current = Join(current_lines);

    const MergeModel model = compare::BuildMergeModel(base, incoming, current);
    total_hunks += model.hunks.size();
    for (const MergeHunk& hunk : model.hunks) conflict_hunks += hunk.conflict ? 1 : 0;
    ExpectChoosingOneSideReproducesIt(model, MergeChoice::Base, base, "base", seed);
    ExpectChoosingOneSideReproducesIt(model, MergeChoice::Incoming, incoming, "incoming", seed);
    ExpectChoosingOneSideReproducesIt(model, MergeChoice::Current, current, "current", seed);
  }
  Expect(total_hunks > 400,
         "the corpus must actually produce hunks, saw " + std::to_string(total_hunks));
  Expect(conflict_hunks > 50,
         "the corpus must reach real conflicts, saw " + std::to_string(conflict_hunks));
}

// MergeChoiceLineViews / MergeChoiceLines / MergeChoiceLineCount are three views
// of one decision. They must agree for every hunk and every choice.
void TestMergeChoiceViewsCountAndLinesAgree() {
  const MergeChoice choices[] = {
      MergeChoice::Base,  MergeChoice::Incoming,          MergeChoice::Current,
      MergeChoice::Both,  MergeChoice::BothCurrentFirst,  MergeChoice::BothIncomingFirst,
  };
  for (std::uint64_t seed = 1; seed <= 300; ++seed) {
    Rng rng(seed ^ 0x5A5A5Aull);
    const std::vector<std::string> base_lines = BuildLines(rng, 2 + rng.Below(12));
    const MergeModel model = compare::BuildMergeModel(Join(base_lines),
                                                      Join(BranchOf(rng, base_lines)),
                                                      Join(BranchOf(rng, base_lines)));
    for (const MergeHunk& hunk : model.hunks) {
      for (const MergeChoice choice : choices) {
        const auto spans = compare::MergeChoiceLineViews(hunk, choice);
        const std::vector<std::string> lines = compare::MergeChoiceLines(hunk, choice);
        const std::size_t count = compare::MergeChoiceLineCount(hunk, choice);
        Expect(count == lines.size(),
               "MergeChoiceLineCount must equal MergeChoiceLines().size()" + Seed(seed));
        Expect(spans.size() == lines.size(),
               "MergeChoiceLineViews must span the same number of lines" + Seed(seed));
        std::size_t index = 0;
        for (const std::string_view line : spans.first) {
          Expect(line == lines[index++], "view and owning form must agree" + Seed(seed));
        }
        for (const std::string_view line : spans.second) {
          Expect(line == lines[index++], "view and owning form must agree" + Seed(seed));
        }
      }
    }
  }
}

// The Both* choices concatenate the two sides in the order their name states --
// but they first collapse the degenerate cases, exactly as VS Code's "Accept
// Both Changes" does: a side that still equals base contributed nothing, so
// "both" is just the other side, and two identical sides are one copy. Assert
// the collapse rules and the ordering separately, because getting the collapse
// wrong duplicates a run into the resolved file.
void TestMergeBothChoicesConcatenateInTheNamedOrder() {
  for (std::uint64_t seed = 1; seed <= 300; ++seed) {
    Rng rng(seed ^ 0xB07Full);
    const std::vector<std::string> base_lines = BuildLines(rng, 2 + rng.Below(10));
    const MergeModel model = compare::BuildMergeModel(Join(base_lines),
                                                      Join(BranchOf(rng, base_lines)),
                                                      Join(BranchOf(rng, base_lines)));
    for (const MergeHunk& hunk : model.hunks) {
      const auto incoming_first = compare::MergeChoiceLines(hunk, MergeChoice::BothIncomingFirst);
      const auto current_first = compare::MergeChoiceLines(hunk, MergeChoice::BothCurrentFirst);
      const auto plain_both = compare::MergeChoiceLines(hunk, MergeChoice::Both);
      Expect(plain_both == current_first,
             "plain Both is the current-first ordering" + Seed(seed));

      const bool sides_agree = hunk.incoming_lines == hunk.current_lines;
      const bool incoming_unchanged = hunk.incoming_lines == hunk.base_lines;
      const bool current_unchanged = hunk.current_lines == hunk.base_lines;
      if (sides_agree || incoming_unchanged || current_unchanged) {
        // One side only: whichever actually carries the change.
        const auto& expected = (sides_agree || current_unchanged) && !incoming_unchanged
                                   ? hunk.incoming_lines
                                   : hunk.current_lines;
        Expect(incoming_first.size() == expected.size(),
               "a degenerate both-choice collapses to one side" + Seed(seed));
        for (std::size_t i = 0; i < expected.size(); ++i) {
          Expect(incoming_first[i] == expected[i],
                 "the collapsed side must be the one that changed" + Seed(seed));
        }
        Expect(current_first == incoming_first,
               "a collapsed both-choice has only one possible order" + Seed(seed));
        continue;
      }

      Expect(incoming_first.size() == hunk.incoming_lines.size() + hunk.current_lines.size(),
             "a real conflict's both-choice carries every line of both sides" + Seed(seed));
      Expect(current_first.size() == incoming_first.size(),
             "both orderings carry the same lines" + Seed(seed));
      for (std::size_t i = 0; i < hunk.incoming_lines.size(); ++i) {
        Expect(incoming_first[i] == hunk.incoming_lines[i],
               "BothIncomingFirst leads with the incoming side" + Seed(seed));
      }
      for (std::size_t i = 0; i < hunk.current_lines.size(); ++i) {
        Expect(current_first[i] == hunk.current_lines[i],
               "BothCurrentFirst leads with the current side" + Seed(seed));
      }
    }
  }
}

// Display rows are what the merge pane paints and navigates. Line numbers must
// increase down each of the three columns, and a row's hunk id must be one the
// display model actually lists.
void TestMergeDisplayRowsAreOrderedAndPointAtRealHunks() {
  for (std::uint64_t seed = 1; seed <= 300; ++seed) {
    Rng rng(seed ^ 0xD15Aull);
    const std::vector<std::string> base_lines = BuildLines(rng, 2 + rng.Below(12));
    const MergeModel model = compare::BuildMergeModel(Join(base_lines),
                                                      Join(BranchOf(rng, base_lines)),
                                                      Join(BranchOf(rng, base_lines)));
    const MergeDisplayModel display = compare::BuildMergeDisplayModel(model);
    int previous_incoming = 0;
    int previous_result = 0;
    int previous_current = 0;
    for (const auto& row : display.rows) {
      if (row.incoming_line != 0) {
        Expect(row.incoming_line > previous_incoming,
               "incoming line numbers must increase down the rows" + Seed(seed));
        previous_incoming = row.incoming_line;
      }
      if (row.result_line != 0) {
        Expect(row.result_line > previous_result,
               "result line numbers must increase down the rows" + Seed(seed));
        previous_result = row.result_line;
      }
      if (row.current_line != 0) {
        Expect(row.current_line > previous_current,
               "current line numbers must increase down the rows" + Seed(seed));
        previous_current = row.current_line;
      }
      Expect(row.hunk < static_cast<int>(model.hunks.size()),
             "a display row must not name a hunk the model does not have" + Seed(seed));
    }
    int previous_end = -1;
    for (std::size_t i = 0; i < display.hunks.size(); ++i) {
      const auto& hunk = display.hunks[i];
      Expect(hunk.start_row <= hunk.end_row && hunk.start_row > previous_end,
             "display hunks must be sorted, disjoint and non-empty" + Seed(seed));
      Expect(hunk.end_row < static_cast<int>(display.rows.size()),
             "a display hunk must stay inside the row vector" + Seed(seed));
      previous_end = hunk.end_row;
    }
  }
}

// A merge of three identical files has nothing to resolve.
void TestIdenticalSidesProduceNoHunks() {
  for (std::uint64_t seed = 1; seed <= 150; ++seed) {
    Rng rng(seed ^ 0x1DE0ull);
    const std::string text = Join(BuildLines(rng, rng.Below(12)));
    const MergeModel model = compare::BuildMergeModel(text, text, text);
    Expect(model.hunks.empty(), "three identical sides have no hunks" + Seed(seed));
    Expect(compare::MergeResultText(model) == text,
         "the result of a no-op merge is the file itself" + Seed(seed));
  }
}

}  // namespace

void RegisterMergeModelPropertyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MergeModelProperty/ChoosingOneSideEverywhereReproducesThatFile",
          TestMergeChoosingOneSideEverywhereReproducesThatFile);
  AddTest(tests, "MergeModelProperty/ChoiceViewsCountAndLinesAgree",
          TestMergeChoiceViewsCountAndLinesAgree);
  AddTest(tests, "MergeModelProperty/BothChoicesConcatenateInTheNamedOrder",
          TestMergeBothChoicesConcatenateInTheNamedOrder);
  AddTest(tests, "MergeModelProperty/DisplayRowsAreOrderedAndPointAtRealHunks",
          TestMergeDisplayRowsAreOrderedAndPointAtRealHunks);
  AddTest(tests, "MergeModelProperty/IdenticalSidesProduceNoHunks",
          TestIdenticalSidesProduceNoHunks);
}

}  // namespace microide::tests
