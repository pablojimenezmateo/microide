// Randomized property tests for the compare (diff) model.
//
// The hand-written cases in CompareModelTests.cpp pin specific diffs. These
// assert the invariants that must hold for EVERY input, over a deterministic
// pseudo-random corpus -- the class of bug that survives a literal-by-literal
// suite because nobody thought to write the literal that breaks it.
//
// The load-bearing invariant is reconstruction: whatever alignment the diff
// chooses, walking the rows must reproduce both input files exactly. A model
// that drops, duplicates, or reorders a line is a data-loss bug in the
// stage/discard path, because the patch generator reads those same rows.

#include "TestSupport.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"
#include "util/StringUtil.h"

namespace microide::tests {
namespace {

using microide::compare::CompareBuildOptions;
using microide::compare::CompareModel;
using microide::compare::CompareRow;
using microide::compare::CompareRowKind;
using microide::compare::DiffOpKind;

// xorshift64*, so the corpus is identical on every machine and every run. A
// std::mt19937 would do, but this keeps the failure message's seed meaningful
// without depending on a library's generation scheme.
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

// A small alphabet of lines, some multi-byte, some whitespace-only, some
// prefixes of each other -- the shapes that make an aligner pick between two
// equally-long alignments.
const std::vector<std::string>& LinePool() {
  static const std::vector<std::string> pool = {
      "",
      " ",
      "\t",
      "   ",
      "alpha",
      "alphabet",
      "beta",
      "beta ",
      " beta",
      "gamma(x)",
      "gamma( x )",
      "}",
      "{",
      "// comment",
      "naïve",
      "naive",
      "日本語のテキスト",
      "日本語",
      "emoji 🙂 here",
      "emoji 🙃 here",
      "áccent",  // combining acute
      "long line ------------------------------------------------------ end",
      "long line ------------------------------------------------------ END",
  };
  return pool;
}

std::string BuildDocument(Rng& rng, std::size_t line_count, bool final_newline) {
  const std::vector<std::string>& pool = LinePool();
  std::string out;
  for (std::size_t i = 0; i < line_count; ++i) {
    out.append(pool[rng.Below(pool.size())]);
    if (i + 1 < line_count || final_newline) out.push_back('\n');
  }
  return out;
}

// Mutate `source` the way an edit session would: drop, insert, replace and swap
// lines. A pure random pair of documents is mostly all-different, which never
// exercises the interesting alignment paths.
std::string MutateDocument(Rng& rng, const std::string& source, bool final_newline) {
  const std::vector<std::string> lines = util::SplitLines(source);
  const std::vector<std::string>& pool = LinePool();
  std::vector<std::string> out;
  out.reserve(lines.size() + 4);
  for (const std::string& line : lines) {
    const std::size_t roll = rng.Below(100);
    if (roll < 12) continue;                                  // delete
    if (roll < 24) {                                          // replace
      out.push_back(pool[rng.Below(pool.size())]);
      continue;
    }
    if (roll < 32) {                                          // insert before
      out.push_back(pool[rng.Below(pool.size())]);
    }
    out.push_back(line);
  }
  if (rng.Chance(30)) out.push_back(pool[rng.Below(pool.size())]);
  std::string text;
  for (std::size_t i = 0; i < out.size(); ++i) {
    text.append(out[i]);
    if (i + 1 < out.size() || final_newline) text.push_back('\n');
  }
  return text;
}

std::string Describe(const std::string& left, const std::string& right, std::uint64_t seed) {
  return " [seed=" + std::to_string(seed) + " left=" + std::to_string(left.size()) +
         "B right=" + std::to_string(right.size()) + "B]";
}

// Walk the rows and rebuild each side's line list. Every non-empty side of a row
// contributes its text; the result must equal SplitLines of the input.
void ExpectRowsReconstructBothSides(const CompareModel& model,
                                    const std::string& left,
                                    const std::string& right,
                                    std::uint64_t seed) {
  const std::vector<std::string> left_lines = util::SplitLines(left);
  const std::vector<std::string> right_lines = util::SplitLines(right);
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  int previous_left_line = 0;
  int previous_right_line = 0;
  for (const CompareRow& row : model.rows) {
    if (row.left_line != 0) {
      Expect(row.left_line > previous_left_line,
             "left line numbers must strictly increase down the rows" +
                 Describe(left, right, seed));
      previous_left_line = row.left_line;
      Expect(left_index < left_lines.size(),
             "the model produced more left lines than the file has" +
                 Describe(left, right, seed));
      Expect(row.left_text == left_lines[left_index],
             "left row text must reproduce the source line, got '" + std::string(row.left_text) +
                 "' want '" + left_lines[left_index] + "'" + Describe(left, right, seed));
      Expect(static_cast<std::size_t>(row.left_line) == left_index + 1,
             "left line number must match its position in the file" +
                 Describe(left, right, seed));
      ++left_index;
    }
    if (row.right_line != 0) {
      Expect(row.right_line > previous_right_line,
             "right line numbers must strictly increase down the rows" +
                 Describe(left, right, seed));
      previous_right_line = row.right_line;
      Expect(right_index < right_lines.size(),
             "the model produced more right lines than the file has" +
                 Describe(left, right, seed));
      Expect(row.right_text == right_lines[right_index],
             "right row text must reproduce the source line, got '" +
                 std::string(row.right_text) + "' want '" + right_lines[right_index] + "'" +
                 Describe(left, right, seed));
      Expect(static_cast<std::size_t>(row.right_line) == right_index + 1,
             "right line number must match its position in the file" +
                 Describe(left, right, seed));
      ++right_index;
    }
  }
  Expect(left_index == left_lines.size(),
         "every left line must appear in exactly one row" + Describe(left, right, seed));
  Expect(right_index == right_lines.size(),
         "every right line must appear in exactly one row" + Describe(left, right, seed));
}

void ExpectRowKindsAgreeWithSides(const CompareModel& model,
                                  const std::string& left,
                                  const std::string& right,
                                  std::uint64_t seed) {
  for (const CompareRow& row : model.rows) {
    const bool has_left = row.left_line != 0;
    const bool has_right = row.right_line != 0;
    Expect(has_left || has_right,
           "a row with neither side is not a row" + Describe(left, right, seed));
    switch (row.kind) {
      case CompareRowKind::Added:
        Expect(!has_left && has_right,
               "an Added row has only a right side" + Describe(left, right, seed));
        break;
      case CompareRowKind::Deleted:
        Expect(has_left && !has_right,
               "a Deleted row has only a left side" + Describe(left, right, seed));
        break;
      case CompareRowKind::Modified:
      case CompareRowKind::Unchanged:
        Expect(has_left && has_right,
               "an Unchanged/Modified row carries both sides" + Describe(left, right, seed));
        break;
    }
  }
}

// Intra-line highlight spans index into the row's own text. A span that runs
// past the end, or splits a UTF-8 sequence, is drawn as a mojibake block or
// reads out of bounds in the renderer.
void ExpectSpansAreInBoundsAndOnBoundaries(const CompareModel& model,
                                           const std::string& left,
                                           const std::string& right,
                                           std::uint64_t seed) {
  const auto check = [&](std::string_view text,
                         const std::vector<microide::compare::CompareTextSpan>& spans,
                         const char* side) {
    std::size_t previous_end = 0;
    for (const auto& span : spans) {
      Expect(span.start <= span.end,
             std::string(side) + " span is inverted" + Describe(left, right, seed));
      Expect(span.end <= text.size(),
             std::string(side) + " span runs past the row text" + Describe(left, right, seed));
      Expect(span.start >= previous_end,
             std::string(side) + " spans must be sorted and disjoint" +
                 Describe(left, right, seed));
      previous_end = span.end;
      Expect(span.start == text.size() || !util::IsUtf8ContinuationByte(
                                              static_cast<unsigned char>(text[span.start])),
             std::string(side) + " span start splits a UTF-8 sequence" +
                 Describe(left, right, seed));
      Expect(span.end == text.size() || !util::IsUtf8ContinuationByte(
                                            static_cast<unsigned char>(text[span.end])),
             std::string(side) + " span end splits a UTF-8 sequence" +
                 Describe(left, right, seed));
    }
  };
  for (const CompareRow& row : model.rows) {
    check(row.left_text, row.left_changed_spans, "left");
    check(row.right_text, row.right_changed_spans, "right");
  }
}

// Hunks index the row vector; a stale or overlapping hunk stages the wrong rows.
void ExpectHunksCoverChangedRowsExactlyOnce(const CompareModel& model,
                                            const std::string& left,
                                            const std::string& right,
                                            std::uint64_t seed) {
  int previous_end = -1;
  for (std::size_t i = 0; i < model.hunks.size(); ++i) {
    const auto& hunk = model.hunks[i];
    Expect(hunk.index == static_cast<int>(i),
           "hunk indices must match their position" + Describe(left, right, seed));
    Expect(hunk.start_row >= 0 && hunk.end_row >= hunk.start_row,
           "a hunk must name a non-empty forward row range" + Describe(left, right, seed));
    Expect(hunk.end_row < static_cast<int>(model.rows.size()),
           "a hunk must stay inside the row vector" + Describe(left, right, seed));
    Expect(hunk.start_row > previous_end,
           "hunks must be sorted and disjoint" + Describe(left, right, seed));
    previous_end = hunk.end_row;
    for (int row = hunk.start_row; row <= hunk.end_row; ++row) {
      Expect(model.rows[row].kind != CompareRowKind::Unchanged,
             "a hunk must not cover an unchanged row" + Describe(left, right, seed));
      Expect(model.rows[row].hunk == hunk.index,
             "a covered row must point back at its hunk" + Describe(left, right, seed));
    }
  }
  for (std::size_t row = 0; row < model.rows.size(); ++row) {
    const bool changed = model.rows[row].kind != CompareRowKind::Unchanged;
    Expect(changed == (model.rows[row].hunk >= 0),
           "exactly the changed rows belong to a hunk" + Describe(left, right, seed));
  }
}

void RunOneCase(std::uint64_t seed, bool ignore_whitespace) {
  Rng rng(seed);
  const bool left_final_newline = rng.Chance(80);
  const bool right_final_newline = rng.Chance(80);
  const std::string left = BuildDocument(rng, rng.Below(14), left_final_newline);
  const std::string right = rng.Chance(70) ? MutateDocument(rng, left, right_final_newline)
                                           : BuildDocument(rng, rng.Below(14), right_final_newline);

  CompareBuildOptions options;
  options.ignore_whitespace = ignore_whitespace;
  const CompareModel model = compare::BuildCompareModel(left, right, options);

  ExpectRowsReconstructBothSides(model, left, right, seed);
  ExpectRowKindsAgreeWithSides(model, left, right, seed);
  ExpectSpansAreInBoundsAndOnBoundaries(model, left, right, seed);
  ExpectHunksCoverChangedRowsExactlyOnce(model, left, right, seed);
}

void TestCompareModelReconstructsBothSidesOverRandomCorpus() {
  for (std::uint64_t seed = 1; seed <= 600; ++seed) {
    RunOneCase(seed, /*ignore_whitespace=*/false);
  }
}

void TestCompareModelHoldsUnderIgnoreWhitespace() {
  for (std::uint64_t seed = 1; seed <= 600; ++seed) {
    RunOneCase(seed, /*ignore_whitespace=*/true);
  }
}

// The op stream is the aligner's own output, one level below the rows. Deletes
// plus Equals must spell the left file; Inserts plus Equals the right one.
void TestLineDiffOpsReplayBothSides() {
  for (std::uint64_t seed = 1; seed <= 600; ++seed) {
    Rng rng(seed ^ 0xABCDEFull);
    const std::string left = BuildDocument(rng, rng.Below(14), true);
    const std::string right = MutateDocument(rng, left, true);
    const std::vector<std::string_view> left_lines = util::SplitLineViews(left);
    const std::vector<std::string_view> right_lines = util::SplitLineViews(right);

    const std::vector<compare::DiffOp> ops = compare::BuildLineDiffOps(left_lines, right_lines);

    std::size_t left_index = 0;
    std::size_t right_index = 0;
    for (const compare::DiffOp& op : ops) {
      switch (op.kind) {
        case DiffOpKind::Equal:
          Expect(left_index < left_lines.size() && right_index < right_lines.size(),
                 "an Equal op consumes one line from each side" + Describe(left, right, seed));
          Expect(op.text == left_lines[left_index],
                 "an Equal op's text is the left line" + Describe(left, right, seed));
          ++left_index;
          ++right_index;
          break;
        case DiffOpKind::Delete:
          Expect(left_index < left_lines.size(),
                 "a Delete op consumes a left line" + Describe(left, right, seed));
          Expect(op.text == left_lines[left_index],
                 "a Delete op's text is the left line" + Describe(left, right, seed));
          ++left_index;
          break;
        case DiffOpKind::Insert:
          Expect(right_index < right_lines.size(),
                 "an Insert op consumes a right line" + Describe(left, right, seed));
          Expect(op.text == right_lines[right_index],
                 "an Insert op's text is the right line" + Describe(left, right, seed));
          ++right_index;
          break;
      }
    }
    Expect(left_index == left_lines.size() && right_index == right_lines.size(),
           "the op stream must consume both files entirely" + Describe(left, right, seed));
  }
}

// Diffing a file against itself is the cheapest possible answer and the one a
// reader notices instantly when it is wrong.
void TestIdenticalDocumentsProduceNoHunks() {
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    Rng rng(seed ^ 0x515151ull);
    const std::string text = BuildDocument(rng, rng.Below(14), rng.Chance(80));
    const CompareModel model = compare::BuildCompareModel(text, text);
    Expect(model.hunks.empty(),
           "a file compared against itself has no hunks" + Describe(text, text, seed));
    for (const CompareRow& row : model.rows) {
      Expect(row.kind == CompareRowKind::Unchanged,
             "every row of a self-compare is unchanged" + Describe(text, text, seed));
    }
    ExpectRowsReconstructBothSides(model, text, text, seed);
  }
}

}  // namespace

void RegisterCompareModelPropertyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CompareModelProperty/ReconstructsBothSidesOverRandomCorpus",
          TestCompareModelReconstructsBothSidesOverRandomCorpus);
  AddTest(tests, "CompareModelProperty/HoldsUnderIgnoreWhitespace",
          TestCompareModelHoldsUnderIgnoreWhitespace);
  AddTest(tests, "CompareModelProperty/LineDiffOpsReplayBothSides",
          TestLineDiffOpsReplayBothSides);
  AddTest(tests, "CompareModelProperty/IdenticalDocumentsProduceNoHunks",
          TestIdenticalDocumentsProduceNoHunks);
}

}  // namespace microide::tests
