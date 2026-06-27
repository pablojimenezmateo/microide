#include "TestSupport.h"

#include <cstdint>
#include <string>
#include <vector>

#include "editor/PieceTree.h"

namespace microide::tests {
namespace {

using microide::editor::PieceTree;

// Naive line-vector oracle: the behavior PieceTree must reproduce exactly.
struct VectorModel {
  std::vector<std::string> lines;

  void Reset(const std::vector<std::string>& value) { lines = value; }

  void ReplaceLineRange(std::size_t start, std::size_t removed,
                        const std::vector<std::string>& inserted) {
    start = std::min(start, lines.size());
    removed = std::min(removed, lines.size() - start);
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(start),
                lines.begin() + static_cast<std::ptrdiff_t>(start + removed));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(start), inserted.begin(),
                 inserted.end());
  }
};

// Deterministic xorshift PRNG so failures reproduce from the seed.
struct Rng {
  std::uint64_t state;
  explicit Rng(std::uint64_t seed) : state(seed ? seed : 0x1234567u) {}
  std::uint64_t Next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }
  std::size_t Below(std::size_t bound) { return bound == 0 ? 0 : Next() % bound; }
};

void ExpectEquivalent(const PieceTree& tree, const VectorModel& model, std::string_view where) {
  Expect(tree.LineCount() == model.lines.size(),
         std::string("piece tree line count mismatch at ").append(where).c_str());
  for (std::size_t i = 0; i < model.lines.size(); ++i) {
    Expect(tree.LineView(i) == model.lines[i],
           std::string("piece tree line content mismatch at ").append(where).c_str());
    Expect(tree.LineLength(i) == model.lines[i].size(),
           std::string("piece tree line length mismatch at ").append(where).c_str());
  }
  // Full materialization must also agree.
  const std::vector<std::string> vectorized = tree.ToVector();
  Expect(vectorized == model.lines,
         std::string("piece tree ToVector mismatch at ").append(where).c_str());
}

std::string MakeLine(Rng& rng) {
  // Lines never contain '\n'. Mix lengths to exercise spanning materialization.
  const std::size_t len = rng.Below(12);
  std::string line;
  line.reserve(len);
  for (std::size_t i = 0; i < len; ++i) {
    line.push_back(static_cast<char>('a' + static_cast<char>(rng.Below(26))));
  }
  return line;
}

void TestPieceTreeBasicSemantics() {
  PieceTree tree;
  Expect(tree.LineCount() == 0, "fresh piece tree is empty");
  Expect(tree.Empty(), "fresh piece tree reports empty");

  tree.Reset({"alpha", "beta", "gamma"});
  Expect(tree.LineCount() == 3, "reset sets line count");
  Expect(tree.LineView(0) == "alpha", "line 0");
  Expect(tree.LineView(2) == "gamma", "line 2");
  Expect(tree.LineLength(1) == 4, "line 1 length");

  tree.InsertLine(1, "INS");
  Expect(tree.LineCount() == 4, "insert grows");
  Expect(tree.LineView(1) == "INS" && tree.LineView(2) == "beta", "insert shifts down");

  tree.SetLine(0, "ALPHA");
  Expect(tree.LineView(0) == "ALPHA", "set replaces in place");

  tree.EraseLine(1);
  Expect(tree.LineCount() == 3 && tree.LineView(1) == "beta", "erase removes line");

  tree.PushBackLine("end");
  Expect(tree.LineView(tree.LineCount() - 1) == "end", "push back appends");

  // One empty line is distinct from an empty document.
  tree.Reset({""});
  Expect(tree.LineCount() == 1 && tree.LineView(0).empty(), "single empty line");
  tree.Reset({});
  Expect(tree.LineCount() == 0, "empty document");
}

void TestPieceTreeMidLineEdits() {
  // Mid-line character edits route through ReplaceLineRange(line,1,{new}) and
  // must keep neighbours untouched -- the core editor edit path.
  PieceTree tree({"one", "two", "three", "four"});
  std::string mid(tree.LineView(2));
  mid.insert(2, "XYZ");
  tree.SetLine(2, mid);
  Expect(tree.LineView(2) == "thXYZree", "in-place mid-line insert");
  Expect(tree.LineView(1) == "two" && tree.LineView(3) == "four", "neighbours intact");
}

void TestPieceTreeRandomizedEquivalence() {
  const std::uint64_t seeds[] = {1u, 7u, 42u, 1337u, 0xC0FFEEu, 0xDEADBEEFu};
  for (std::uint64_t seed : seeds) {
    Rng rng(seed);
    PieceTree tree;
    VectorModel model;

    // Seed with an initial document.
    std::vector<std::string> initial;
    const std::size_t initial_lines = rng.Below(40);
    for (std::size_t i = 0; i < initial_lines; ++i) initial.push_back(MakeLine(rng));
    tree.Reset(initial);
    model.Reset(initial);
    ExpectEquivalent(tree, model, "after reset");

    for (int step = 0; step < 400; ++step) {
      const std::size_t n = model.lines.size();
      const std::size_t start = rng.Below(n + 1);
      const std::size_t removed = rng.Below((n - std::min(start, n)) + 1);
      const std::size_t insert_count = rng.Below(4);
      std::vector<std::string> inserted;
      for (std::size_t i = 0; i < insert_count; ++i) inserted.push_back(MakeLine(rng));

      tree.ReplaceLineRange(start, removed, inserted);
      model.ReplaceLineRange(start, removed, inserted);
      ExpectEquivalent(tree, model, "after random replace");
    }
  }
}

void TestPieceTreeSliceLines() {
  PieceTree tree({"l0", "l1", "l2", "l3", "l4"});
  const std::vector<std::string> slice = tree.SliceLines(1, 4);
  Expect(slice.size() == 3 && slice[0] == "l1" && slice[2] == "l3", "slice lines [1,4)");
  Expect(tree.SliceLines(3, 3).empty(), "empty slice");
  Expect(tree.SliceLines(4, 99).size() == 1, "slice clamps to end");
}

// ResetFromText (the Phase 4 large-file load fast path) must be exactly
// equivalent to splitting the same canonical '\n'-joined bytes into lines and
// feeding them through Reset -- it just skips the intermediate vector.
void TestPieceTreeResetFromTextMatchesReset() {
  const std::vector<std::string> cases = {
      "",                       // empty file -> single empty line
      "solo",                   // no trailing newline, one line
      "a\nb\nc",                // multiple lines, no trailing newline
      "a\nb\nc\n",              // trailing newline -> final empty line
      "\n",                     // single newline -> two empty lines
      "\n\n\n",                 // only newlines
      std::string("a\0b\nc", 5) // embedded NUL is in-line content
  };
  for (const std::string& text : cases) {
    // Split exactly the way the canonical model joins: on '\n' only.
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == '\n') {
        lines.emplace_back(text.substr(start, i - start));
        start = i + 1;
      }
    }

    PieceTree from_text;
    from_text.ResetFromText(text);
    PieceTree from_lines(lines);

    Expect(from_text.LineCount() == from_lines.LineCount(),
           "ResetFromText line count matches Reset");
    Expect(from_text.LineCount() == lines.size(),
           "ResetFromText derives the canonical line count");
    for (std::size_t i = 0; i < from_text.LineCount(); ++i) {
      Expect(from_text.LineView(i) == from_lines.LineView(i),
             "ResetFromText line content matches Reset");
    }
    Expect(from_text.ToVector() == from_lines.ToVector(),
           "ResetFromText materializes the same document");
  }
}

}  // namespace

void RegisterPieceTreeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PieceTree/BasicSemantics", TestPieceTreeBasicSemantics);
  AddTest(tests, "PieceTree/MidLineEdits", TestPieceTreeMidLineEdits);
  AddTest(tests, "PieceTree/RandomizedEquivalence", TestPieceTreeRandomizedEquivalence);
  AddTest(tests, "PieceTree/SliceLines", TestPieceTreeSliceLines);
  AddTest(tests, "PieceTree/ResetFromTextMatchesReset", TestPieceTreeResetFromTextMatchesReset);
}

}  // namespace microide::tests
