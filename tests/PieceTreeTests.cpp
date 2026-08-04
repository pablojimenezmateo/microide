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

// LineView keeps ascending-walk state — which node the last resolved newline sat
// in, its byte base, and its position in that buffer's newline array — so a
// sequential reader pays no tree descent per line. TestPieceTreeRandomizedEquivalence
// walks lines in order, which is the path that state is built for; the risk this
// adds is the OTHER order, where the state describes a piece unrelated to the line
// being asked for and must be declined rather than trusted.
//
// Fuzz random access orders over a fragmented tree, interleaved with edits, against
// the naive model.
void TestPieceTreeRandomAccessOrderMatchesModel() {
  const std::uint64_t seeds[] = {5u, 23u, 0xBEEFu, 0x1234u};
  for (std::uint64_t seed : seeds) {
    Rng rng(seed);
    PieceTree tree;
    VectorModel model;
    std::vector<std::string> initial;
    const std::size_t initial_lines = 20 + rng.Below(40);
    for (std::size_t i = 0; i < initial_lines; ++i) initial.push_back(MakeLine(rng));
    tree.Reset(initial);
    model.Reset(initial);

    for (int step = 0; step < 120; ++step) {
      // Fragment further: each edit spawns add-buffer pieces, so the walk state's
      // "still inside the same piece" test starts failing in interesting places.
      const std::size_t n = model.lines.size();
      const std::size_t start = rng.Below(n + 1);
      const std::size_t removed = rng.Below((n - std::min(start, n)) + 1);
      std::vector<std::string> inserted;
      for (std::size_t i = 0; i < rng.Below(3); ++i) inserted.push_back(MakeLine(rng));
      tree.ReplaceLineRange(start, removed, inserted);
      model.ReplaceLineRange(start, removed, inserted);

      const std::size_t count = model.lines.size();
      if (count == 0) {
        continue;
      }
      // Random order, including repeats and backwards runs.
      for (int probe = 0; probe < 40; ++probe) {
        const std::size_t line = rng.Below(count);
        Expect(tree.LineView(line) == model.lines[line],
               "random-order LineView must match the model — ascending-walk state must "
               "never answer for a line it does not describe");
        Expect(tree.LineLength(line) == model.lines[line].size(),
               "random-order LineLength must match the model");
      }
      // A descending sweep, which is the exact inverse of what the state expects.
      for (std::size_t line = count; line-- > 0;) {
        Expect(tree.LineView(line) == model.lines[line],
               "descending LineView must match the model");
      }
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

// SliceLines / ToVector share one single-pass in-order extractor (ExtractLineRange)
// that splits piece bytes on '\n' with an early stop for partial ranges and a
// post-walk push for the final unterminated line. Fuzz every (begin, end) slice of
// a heavily-fragmented multi-piece tree against the naive oracle so the early-stop
// boundary, the final-line push, and slices that straddle piece boundaries all stay
// exact.
void TestPieceTreeSliceLinesEquivalence() {
  const std::uint64_t seeds[] = {3u, 19u, 0xABCDu, 0xFEEDu};
  for (std::uint64_t seed : seeds) {
    Rng rng(seed);
    PieceTree tree;
    VectorModel model;
    std::vector<std::string> initial;
    const std::size_t initial_lines = rng.Below(30);
    for (std::size_t i = 0; i < initial_lines; ++i) initial.push_back(MakeLine(rng));
    tree.Reset(initial);
    model.Reset(initial);

    // Fragment the tree with many small edits (each spawns add-buffer pieces).
    for (int step = 0; step < 120; ++step) {
      const std::size_t n = model.lines.size();
      const std::size_t start = rng.Below(n + 1);
      const std::size_t removed = rng.Below((n - std::min(start, n)) + 1);
      std::vector<std::string> inserted;
      for (std::size_t i = 0; i < rng.Below(3); ++i) inserted.push_back(MakeLine(rng));
      tree.ReplaceLineRange(start, removed, inserted);
      model.ReplaceLineRange(start, removed, inserted);

      const std::size_t lc = tree.LineCount();
      for (std::size_t begin = 0; begin <= lc + 1; ++begin) {
        for (std::size_t end = begin; end <= lc + 1; ++end) {
          std::vector<std::string> expected;
          if (begin < end && begin < lc) {
            const std::size_t clamped_end = std::min(end, lc);
            expected.assign(model.lines.begin() + static_cast<std::ptrdiff_t>(begin),
                            model.lines.begin() + static_cast<std::ptrdiff_t>(clamped_end));
          }
          Expect(tree.SliceLines(begin, end) == expected, "SliceLines matches oracle slice");
        }
      }
    }
  }
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

// Regression: the append-only add_ buffer indexes bytes with 32-bit offsets, so
// it must be compactable before add_.size() can cross 4 GiB and wrap. Compaction
// is representation-only — it materializes the live document into the original
// buffer and empties add_, preserving content and line count. (The overflow
// itself is impractical to allocate, so exercise the compaction path directly.)
void TestPieceTreeAddBufferCompaction() {
  PieceTree tree({"alpha", "beta", "gamma"});
  // Each mutator routes new text through InsertText -> AppendToAdd, growing add_.
  tree.SetLine(0, "alpha-edited");
  tree.InsertLine(1, "inserted");
  tree.ReplaceLineRange(2, 1, {"beta-1", "beta-2"});
  tree.PushBackLine("delta");
  Expect(tree.AddBufferSizeForTesting() > 0, "edits should have grown the add buffer");

  const std::vector<std::string> before = tree.ToVector();
  const std::size_t before_lines = tree.LineCount();
  const std::size_t before_bytes = tree.ByteSize();

  tree.CompactAddBufferForTesting();

  Expect(tree.AddBufferSizeForTesting() == 0, "compaction should empty the add buffer");
  Expect(tree.ToVector() == before, "compaction must preserve content exactly");
  Expect(tree.LineCount() == before_lines, "compaction must preserve the line count");
  Expect(tree.ByteSize() == before_bytes, "compaction must preserve the byte size");

  // Edits after compaction must still apply correctly (add_ re-grows from zero).
  tree.SetLine(0, "post-compact");
  Expect(tree.AddBufferSizeForTesting() > 0, "post-compaction edits should re-grow add_");
  std::vector<std::string> expected = before;
  expected[0] = "post-compact";
  Expect(tree.ToVector() == expected, "edits after compaction must apply correctly");
}

// Regression (TD-2026-08-04-130): `add_` is append-only, so repeated large edits
// accumulated dead history forever — a session doing sustained multi-line editing
// grew resident memory with no upper bound and no way to get it back short of
// restarting. The 4 GiB overflow guard is a correctness backstop that in practice
// never fires. Compaction is now also driven by memory pressure, so the dead
// history stays proportional to the LIVE document.
//
// The shape that made this hard to see: `add_` doubling itself is one allocation
// and one free, so allocation counts stay balanced while the bytes climb.
// Assert on the buffer size, which is the thing that actually grows.
void TestPieceTreeAddBufferStaysBoundedUnderRepeatedLargeEdits() {
  std::vector<std::string> lines;
  lines.reserve(2000);
  for (std::size_t i = 0; i < 2000; ++i) {
    lines.push_back("line " + std::to_string(i) + " with enough content to be worth bytes");
  }
  PieceTree tree(lines);

  // Rewrite a 1,000-line window over and over, exactly what toggle-line-comment on
  // a large selection does. Each pass appends ~the window's bytes to add_.
  std::vector<std::string> replacement(lines.begin(), lines.begin() + 1000);
  const auto run_passes = [&](int count, int base_pass) {
    std::size_t peak = 0;
    for (int pass = 0; pass < count; ++pass) {
      replacement[0] = "pass " + std::to_string(base_pass + pass);
      tree.ReplaceLineRange(500, 1000, replacement);
      peak = std::max(peak, tree.AddBufferSizeForTesting());
    }
    return peak;
  };

  // Boundedness is asserted against the buffer's OWN earlier peak rather than
  // against the trigger's constants, so the test cannot drift out of agreement
  // with them: if the dead history is bounded, running four times as long cannot
  // raise the high-water mark. Without the memory-pressure trigger the second
  // window's peak is ~4x the first, because add_ only ever grows.
  const std::size_t early_peak = run_passes(200, 0);
  const std::size_t late_peak = run_passes(800, 200);
  // A small slack, because the replacement text is not byte-identical across passes
  // (the pass number is in it), so the exact byte at which the threshold is crossed
  // moves by a few bytes. Measured: 4,216,850 against 4,216,860. Unbounded growth
  // would put the second window's peak at ~4x the first.
  Expect(late_peak <= early_peak + 64 * 1024,
         "repeated large edits must not raise the add buffer's high-water mark");

  // And the content is still exactly right: compaction is representation-only, and
  // it now happens in the middle of an edit rather than only from a test seam.
  const std::vector<std::string> materialized = tree.ToVector();
  Expect(materialized.size() == lines.size(),
         "compaction during an edit must preserve the line count");
  Expect(materialized[500] == "pass 999",
         "compaction during an edit must preserve the edited content");
  Expect(materialized[499] == lines[499] && materialized[1500] == lines[1500],
         "compaction during an edit must leave the untouched lines alone");
}

// Regression: a spanning line (content crossing a piece boundary, the shape
// mid-line character editing produces) is materialized into a per-index cache
// slot. A second LineView() for the SAME index must return the already-cached
// slot unchanged -- re-running clear()+CopyRange could reallocate the slot's
// buffer and dangle a view returned by the earlier call. Assert the two views
// share the same backing pointer (proving no reallocation) and equal content.
void TestPieceTreeSpanningLineViewStableAcrossReReads() {
  PieceTree tree({"abcdef", "second"});
  // Byte-level mid-line insert at column 3 of line 0: "abc" | "XYZ" | "def",
  // so LineView(0) spans three pieces and takes the non-contiguous slow path.
  tree.InsertTextForTesting(3, "XYZ");
  const std::string_view first = tree.LineView(0);
  Expect(first == "abcXYZdef", "spanning line materializes correctly");
  // Re-read the same index. With the fix this returns the same slot; without it
  // the slot is cleared+rebuilt and `first` would dangle.
  const std::string_view second = tree.LineView(0);
  Expect(second == "abcXYZdef", "same-index re-read yields identical content");
  Expect(first.data() == second.data(),
         "same-index re-read must not reallocate the cache slot (first view stays valid)");
  Expect(first == "abcXYZdef", "the first view is still valid after the second read");
  // The untouched neighbour line is unaffected.
  Expect(tree.LineView(1) == "second", "neighbour line intact");
}

// TD-2026-07-16-35: a mutation that would push the live document past the byte ceiling
// must be refused as a no-op (leaving content + line count intact) rather than wrap the
// uint32 subtree_length and corrupt the tree. Exercised via a lowered test ceiling.
void TestPieceTreeLiveDocumentByteCeiling() {
  PieceTree tree({"hello", "world"});
  const std::vector<std::string> before = tree.ToVector();
  const std::size_t before_bytes = tree.ByteSize();
  const std::size_t before_lines = tree.LineCount();

  // Lower the ceiling to just below the current size + a small insert.
  tree.SetMaxLiveDocumentBytesForTesting(static_cast<std::uint32_t>(before_bytes + 2));

  // A small insert that fits under the ceiling still applies.
  tree.PushBackLine("x");  // +2 bytes ("\n" + "x")
  Expect(!tree.LastMutationRejectedForByteCeiling(), "an in-budget mutation applies");
  Expect(tree.LineCount() == before_lines + 1, "the in-budget mutation grew the document");

  // Now any further growth exceeds the ceiling and must be refused as a no-op.
  const std::vector<std::string> at_cap = tree.ToVector();
  const std::size_t at_cap_bytes = tree.ByteSize();
  tree.PushBackLine("this would overflow the lowered ceiling");
  Expect(tree.LastMutationRejectedForByteCeiling(),
         "a mutation past the byte ceiling is rejected");
  Expect(tree.ToVector() == at_cap, "a rejected mutation leaves content unchanged");
  Expect(tree.ByteSize() == at_cap_bytes, "a rejected mutation leaves byte size unchanged");

  // A delete (shrinking) is always allowed even at the ceiling.
  tree.EraseLine(0);
  Expect(!tree.LastMutationRejectedForByteCeiling(), "a shrinking mutation is never refused");
  Expect(tree.LineCount() == at_cap.size() - 1, "the delete applied");
}

// AppendWholeText must be byte-identical to the '\n'-join of every line — that
// is the contract the in-file find/replace surface relies on, and it replaced a
// hand-rolled per-line join. Exercised on a tree that has been mid-line edited
// so lines genuinely span pieces (the case where the per-line path also had to
// materialize into the line cache).
void TestPieceTreeAppendWholeTextMatchesLineJoin() {
  const auto join = [](const PieceTree& tree) {
    std::string joined;
    for (std::size_t i = 0; i < tree.LineCount(); ++i) {
      if (i != 0) {
        joined.push_back('\n');
      }
      joined.append(tree.LineView(i));
    }
    return joined;
  };

  PieceTree empty;
  std::string out;
  empty.AppendWholeText(out);
  Expect(out == join(empty), "empty document: whole text matches the line join");

  PieceTree tree({"alpha", "", "gamma delta", "epsilon"});
  out.clear();
  tree.AppendWholeText(out);
  Expect(out == join(tree), "fresh document: whole text matches the line join");
  Expect(out.size() == tree.ByteSize(), "whole text length equals ByteSize()");

  // Force lines to span multiple pieces.
  tree.InsertTextForTesting(2, "XYZ");
  tree.InsertTextForTesting(15, "-mid-");
  tree.InsertLine(1, "inserted");
  out.clear();
  tree.AppendWholeText(out);
  Expect(out == join(tree), "after mid-line edits: whole text still matches the line join");
  Expect(out.size() == tree.ByteSize(), "whole text length still equals ByteSize()");

  // Appends rather than assigns: an existing prefix must survive.
  std::string prefixed = "PREFIX:";
  tree.AppendWholeText(prefixed);
  Expect(prefixed == "PREFIX:" + out, "AppendWholeText appends, it does not overwrite");
}

}  // namespace

// LineView/LineLength memoize the byte offset of the last line start they
// resolved, so an ascending walk costs one tree descent per line instead of two.
// The hazard that buys is a stale memo surviving a mutation: reads after an edit
// would then resolve line starts against the OLD document layout.
//
// Priming matters, and is subtle: LineView(i) asks for start(i) then start(i+1),
// so it leaves the memo holding i + 1. A test that mutates and then reads from
// line 0 upward never consults a stale entry (index 0 misses, and every later
// index is re-primed on the way). Each case below therefore primes a specific
// index, mutates in a way that MOVES that index's start byte, and reads exactly
// that index first. Verified to fail if BumpRevision stops clearing the memo.
void TestPieceTreeLineStartMemoInvalidation() {
  const std::vector<std::string> base = {"alpha", "bravo", "charlie", "delta", "echo"};

  // 1. Insertion before the memoized index shifts its start byte.
  {
    PieceTree tree(base);
    (void)tree.LineView(1);            // memo now holds index 2
    tree.InsertLine(0, "prefix-line");  // every start shifts right
    Expect(tree.LineView(2) == "bravo",
           "an insert before the memoized line must invalidate its cached start");
  }

  // 2. Deletion before the memoized index shifts it the other way.
  {
    PieceTree tree(base);
    (void)tree.LineView(2);  // memo now holds index 3
    tree.EraseLine(0);
    Expect(tree.LineView(3) == "echo",
           "an erase before the memoized line must invalidate its cached start");
  }

  // 3. An in-place edit that CHANGES LENGTH moves every following start.
  {
    PieceTree tree(base);
    (void)tree.LineView(1);  // memo now holds index 2
    tree.SetLine(0, "a-much-longer-first-line");
    Expect(tree.LineView(2) == "charlie",
           "a length-changing edit must invalidate the cached start after it");
  }

  // 4. LineLength primes and reads the same memo as LineView.
  {
    PieceTree tree(base);
    (void)tree.LineLength(1);  // memo now holds index 2
    tree.InsertLine(0, "prefix-line");
    Expect(tree.LineLength(2) == 5, "LineLength must not read a stale cached start");
  }

  // 5. Reset / ResetFromText rebuild through RebuildFromOriginal rather than the
  //    normal mutation path — the path that used to clear the spanning-line cache
  //    inline instead of via BumpRevision.
  {
    PieceTree tree(base);
    (void)tree.LineView(1);  // memo now holds index 2
    tree.Reset({"x", "much longer second line here", "z"});
    Expect(tree.LineView(2) == "z", "Reset must invalidate the line-start memo");
  }
  {
    PieceTree tree(base);
    (void)tree.LineView(1);  // memo now holds index 2
    tree.ResetFromText("one\ntwo-is-much-longer\nthree");
    Expect(tree.LineView(2) == "three", "ResetFromText must invalidate the line-start memo");
  }

  // 6. Out-of-order reads must be correct too: the memo holds a single entry, so
  //    a jumping walk misses it constantly and must fall back to a real descent.
  {
    PieceTree tree(base);
    for (const std::size_t index : {std::size_t{4}, std::size_t{0}, std::size_t{3},
                                    std::size_t{1}, std::size_t{4}, std::size_t{2}}) {
      Expect(tree.LineView(index) == base[index],
             "out-of-order reads must resolve independently of the memo");
      Expect(tree.LineLength(index) == base[index].size(),
             "out-of-order LineLength must agree");
    }
  }
}

void RegisterPieceTreeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PieceTree/LineStartMemoInvalidation", TestPieceTreeLineStartMemoInvalidation);
  AddTest(tests, "PieceTree/LiveDocumentByteCeiling", TestPieceTreeLiveDocumentByteCeiling);
  AddTest(tests, "PieceTree/BasicSemantics", TestPieceTreeBasicSemantics);
  AddTest(tests, "PieceTree/AddBufferCompaction", TestPieceTreeAddBufferCompaction);
  AddTest(tests, "PieceTree/AddBufferStaysBoundedUnderRepeatedLargeEdits",
          TestPieceTreeAddBufferStaysBoundedUnderRepeatedLargeEdits);
  AddTest(tests, "PieceTree/MidLineEdits", TestPieceTreeMidLineEdits);
  AddTest(tests, "PieceTree/RandomizedEquivalence", TestPieceTreeRandomizedEquivalence);
  AddTest(tests, "PieceTree/RandomAccessOrderMatchesModel",
          TestPieceTreeRandomAccessOrderMatchesModel);
  AddTest(tests, "PieceTree/SliceLines", TestPieceTreeSliceLines);
  AddTest(tests, "PieceTree/SliceLinesEquivalence", TestPieceTreeSliceLinesEquivalence);
  AddTest(tests, "PieceTree/ResetFromTextMatchesReset", TestPieceTreeResetFromTextMatchesReset);
  AddTest(tests, "PieceTree/AppendWholeTextMatchesLineJoin",
          TestPieceTreeAppendWholeTextMatchesLineJoin);
  AddTest(tests, "PieceTree/SpanningLineViewStableAcrossReReads",
          TestPieceTreeSpanningLineViewStableAcrossReReads);
}

}  // namespace microide::tests
