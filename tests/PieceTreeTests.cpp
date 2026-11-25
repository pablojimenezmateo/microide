#include "TestSupport.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <random>
#include <string>
#include <vector>

#include "editor/CaretNeighborhood.h"
#include "editor/LineBlob.h"
#include "editor/PieceTree.h"
#include "editor/TextBuffer.h"
#include "perf/AllocationCounter.h"
#include "util/PerformanceCounters.h"

namespace microide::tests {
namespace {

using microide::editor::PieceTree;

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
  const std::vector<std::string> vectorized = tree.ToVector();
  Expect(vectorized == model.lines,
         std::string("piece tree ToVector mismatch at ").append(where).c_str());
}

std::string MakeLine(Rng& rng) {
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

  tree.Reset({""});
  Expect(tree.LineCount() == 1 && tree.LineView(0).empty(), "single empty line");
  tree.Reset({});
  Expect(tree.LineCount() == 0, "empty document");
}

void TestPieceTreeMidLineEdits() {
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
      std::string window_scratch;
      for (int probe = 0; probe < 40; ++probe) {
        const std::size_t line = rng.Below(count);
        const std::string& expected_line = model.lines[line];
        const std::size_t start = rng.Below(expected_line.size() + 2);
        const std::size_t len = rng.Below(expected_line.size() + 2);
        const std::string expected_window =
            start >= expected_line.size() ? std::string{} : expected_line.substr(start, len);
        Expect(tree.LineWindow(line, start, len, window_scratch) == expected_window,
               "random-order LineWindow must match the model's slice");
        Expect(tree.LineView(line) == expected_line,
               "random-order LineView must match the model — ascending-walk state must "
               "never answer for a line it does not describe");
        Expect(tree.LineLength(line) == expected_line.size(),
               "random-order LineLength must match the model");
      }
      for (std::size_t line = count; line-- > 0;) {
        Expect(tree.LineView(line) == model.lines[line],
               "descending LineView must match the model");
      }
    }
  }
}

void TestPieceTreeExtractIntoBlobSizesItsByteBuffer() {
  std::vector<std::string> lines;
  lines.reserve(2000);
  for (int i = 0; i < 2000; ++i) {
    lines.push_back(std::string(60, static_cast<char>('a' + (i % 26))));
  }
  PieceTree tree(lines);
  editor::LineBlob warmup;
  tree.AppendLines(0, lines.size(), warmup);
  editor::LineBlob blob;
#if MICROIDE_PERF_HARNESS_BUILD
  const perf::AllocationSnapshot before = perf::Allocations::Snapshot();
#endif
  tree.AppendLines(0, lines.size(), blob);
#if MICROIDE_PERF_HARNESS_BUILD
  const perf::AllocationDelta delta = perf::Allocations::DeltaSince(before);
  Expect(delta.allocations <= 4,
         "extracting into a blob should reserve its byte buffer, not grow into it");
#endif
  Expect(blob.size() == lines.size(), "blob extract should produce every line");
  Expect(blob[0] == lines[0] && blob[lines.size() - 1] == lines[lines.size() - 1],
         "blob extract should reproduce the buffer's lines");
}

void TestPieceTreeSmallBlobIsAllocationFree() {
  PieceTree tree({"one", "two", "three", "four", "five"});
  editor::LineBlob warmup;
  tree.AppendLines(0, 3, warmup);
#if MICROIDE_PERF_HARNESS_BUILD
  const perf::AllocationSnapshot before = perf::Allocations::Snapshot();
#endif
  editor::LineBlob blob;
  blob.reserve_lines(3);
  tree.AppendLines(1, 4, blob);
#if MICROIDE_PERF_HARNESS_BUILD
  const perf::AllocationDelta delta = perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0,
         "a blob of a few short lines must fit its inline offset slots and the "
         "string's own small buffer, and allocate nothing");
#endif
  Expect(blob.size() == 3 && blob[0] == "two" && blob[2] == "four",
         "the inline-capacity blob must still hold the right lines");

  editor::LineBlob spilled;
  for (int i = 0; i < 40; ++i) {
    spilled.push_back(std::string(1, static_cast<char>('a' + (i % 26))));
  }
  Expect(spilled.size() == 40, "a blob past its inline slots must keep every line");
  Expect(spilled[0] == "a" && spilled[39] == std::string(1, static_cast<char>('a' + (39 % 26))),
         "spilling to the heap must not disturb the offsets already recorded");
}

void TestPieceTreeSliceLines() {
  PieceTree tree({"l0", "l1", "l2", "l3", "l4"});
  const std::vector<std::string> slice = tree.SliceLines(1, 4);
  Expect(slice.size() == 3 && slice[0] == "l1" && slice[2] == "l3", "slice lines [1,4)");
  Expect(tree.SliceLines(3, 3).empty(), "empty slice");
  Expect(tree.SliceLines(4, 99).size() == 1, "slice clamps to end");
}

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

void TestPieceTreeResetFromTextMatchesReset() {
  const std::vector<std::string> cases = {
      "",
      "solo",
      "a\nb\nc",
      "a\nb\nc\n",
      "\n",
      "\n\n\n",
      std::string("a\0b\nc", 5)
  };
  for (const std::string& text : cases) {
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

void TestPieceTreeAddBufferCompaction() {
  PieceTree tree({"alpha", "beta", "gamma"});
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

  tree.SetLine(0, "post-compact");
  Expect(tree.AddBufferSizeForTesting() > 0, "post-compaction edits should re-grow add_");
  std::vector<std::string> expected = before;
  expected[0] = "post-compact";
  Expect(tree.ToVector() == expected, "edits after compaction must apply correctly");
}

void TestPieceTreeAddBufferStaysBoundedUnderRepeatedLargeEdits() {
  std::vector<std::string> lines;
  lines.reserve(2000);
  for (std::size_t i = 0; i < 2000; ++i) {
    lines.push_back("line " + std::to_string(i) + " with enough content to be worth bytes");
  }
  PieceTree tree(lines);

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

  const std::size_t early_peak = run_passes(200, 0);
  const std::size_t late_peak = run_passes(800, 200);
  Expect(late_peak <= early_peak + 64 * 1024,
         "repeated large edits must not raise the add buffer's high-water mark");

  const std::vector<std::string> materialized = tree.ToVector();
  Expect(materialized.size() == lines.size(),
         "compaction during an edit must preserve the line count");
  Expect(materialized[500] == "pass 999",
         "compaction during an edit must preserve the edited content");
  Expect(materialized[499] == lines[499] && materialized[1500] == lines[1500],
         "compaction during an edit must leave the untouched lines alone");
}

void TestPieceTreeAddChunksNeverMoveStoredHistory() {
  PieceTree tree({"seed"});
  const std::string payload(64, 'x');
  std::vector<const char*> observed;
  observed.reserve(4000);
  for (int i = 0; i < 4000; ++i) {
    tree.PushBackLine(payload);
    observed.push_back(tree.LineView(tree.LineCount() - 1).data());
  }
  Expect(tree.LineCount() == 4001, "every append landed");

  for (const char* view : observed) {
    Expect(std::string_view(view, payload.size()) == payload,
           "text already in a chunk keeps its address and its bytes");
  }

  const std::size_t chunks = tree.AddChunkCountForTesting();
  Expect(chunks >= 4 && chunks <= 16,
         "chunk count tracks bytes typed / chunk cap, not the append count");
}

void TestPieceTreeAddChunkHoldsAnOversizedInsertContiguously() {
  PieceTree tree({"seed"});
  const std::string huge(200u * 1024u, 'q');
  tree.PushBackLine(huge);
  Expect(tree.LineCount() == 2, "the oversized insert is one line");
  const std::string_view view = tree.LineView(1);
  Expect(view.size() == huge.size() && view == huge,
         "an insert larger than the chunk cap reads back intact");
  Expect(tree.LineOwnedIfMaterialized(1) == nullptr,
         "an oversized insert stays one contiguous piece (no spanning-line copy)");
  tree.PushBackLine("after");
  Expect(tree.LineView(2) == "after", "a small append after an oversized chunk lands correctly");
  Expect(tree.LineView(1) == huge, "the oversized chunk is untouched by later appends");
}

void TestPieceTreeSpanningLineViewStableAcrossReReads() {
  PieceTree tree({"abcdef", "second"});
  tree.InsertTextForTesting(3, "XYZ");
  const std::string_view first = tree.LineView(0);
  Expect(first == "abcXYZdef", "spanning line materializes correctly");
  const std::string_view second = tree.LineView(0);
  Expect(second == "abcXYZdef", "same-index re-read yields identical content");
  Expect(first.data() == second.data(),
         "same-index re-read must not reallocate the cache slot (first view stays valid)");
  Expect(first == "abcXYZdef", "the first view is still valid after the second read");
  Expect(tree.LineView(1) == "second", "neighbour line intact");
}

void TestPieceTreeLineWindowMatchesSubstring() {
  PieceTree tree({"abcdefghij", "second line", "caf\xc3\xa9 na\xc3\xafve"});
  tree.InsertTextForTesting(4, "WXYZ");

  std::string scratch;
  for (std::size_t line = 0; line < tree.LineCount(); ++line) {
    const std::string whole(tree.LineView(line));
    for (std::size_t start = 0; start <= whole.size() + 2; ++start) {
      for (std::size_t len = 0; len <= whole.size() + 2; ++len) {
        const std::string_view window = tree.LineWindow(line, start, len, scratch);
        const std::string expected =
            start >= whole.size() ? std::string{} : whole.substr(start, len);
        Expect(window == expected, "LineWindow must equal the same slice of the whole line");
      }
    }
  }
}

void TestPieceTreeLineWindowDoesNotMaterializeTheLine() {
  PieceTree tree({std::string(4096, 'a'), "tail"});
  tree.InsertTextForTesting(2000, "MID");

  util::ResetPerformanceCounters();
  std::string scratch;
  Expect(tree.LineWindow(0, 0, 16, scratch) == std::string(16, 'a'), "prefix window reads");
  Expect(tree.LineWindow(0, 1996, 8, scratch) == "aaaaMIDa", "straddling window reads");
  Expect(tree.LineWindow(0, 4090, 64, scratch) == std::string(9, 'a'), "suffix window clamps");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializations) == 0,
         "a bounded window must never materialize the whole line");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes) == 0,
         "a bounded window must not copy line-sized byte counts");

  (void)tree.LineView(0);
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializations) == 1,
         "reading the whole spanning line still materializes it (control)");
  util::ResetPerformanceCounters();
}

void TestCaretNeighborhoodMatchesWholeLineReading() {
  const std::vector<std::string> lines = {
      "caf\xc3\xa9()na\xc3\xafve[]\xe2\x82\xac{}xyzzy\tend",
      "",
      "a",
      "\xf0\x9f\x98\x80!",
      "ab\xf0\x9f\x98\x80" "cd",  // NOLINT(bugprone-suspicious-missing-comma)
      std::string("x\x7f\x80\x81\x82\x83y\xe2\x82\xacz\xe2\x82!\xf0\x9f\x98\x80\x80", 19),
  };
  std::string content;
  for (const std::string& line : lines) {
    content += line;
    content += '\n';
  }
  microide::editor::TextBuffer buffer;
  buffer.ResetFromText(content);
  for (std::size_t line = 0; line < buffer.size(); ++line) {
    const std::size_t length = buffer.LineLength(line);
    if (length >= 2) {
      const std::size_t mid = length / 2;
      buffer.ReplaceTextRange(line, mid, line, mid, "Z");
      buffer.ReplaceTextRange(line, mid, line, mid + 1, "");
    }
  }

  for (std::size_t line = 0; line < buffer.size(); ++line) {
    const std::string whole(buffer.LineView(line));
    for (std::size_t column = 0; column <= whole.size() + 2; ++column) {
      const microide::editor::CaretNeighborhood at =
          microide::editor::ReadCaretNeighborhood(buffer, line, column);
      const std::size_t expected_clamped =
          microide::editor::TextLayout::ClampTextColumn(whole, column);
      const std::string where =
          " (line " + std::to_string(line) + ", column " + std::to_string(column) + ")";
      Expect(at.line_length == whole.size(), "line_length must match the whole line" + where);
      Expect(at.clamped_column == expected_clamped,
             "clamped_column must match ClampTextColumn over the whole line" + where);
      Expect(at.at_end == (expected_clamped >= whole.size()),
             "at_end must match the whole-line end test" + where);
      if (!at.at_end) {
        Expect(at.next == whole[expected_clamped], "next must be the byte at the clamp" + where);
      }
      Expect(at.has_prev == (expected_clamped > 0), "has_prev must match the clamp" + where);
      if (at.has_prev) {
        Expect(at.prev == whole[expected_clamped - 1],
               "prev must be the byte before the clamp" + where);
        const std::size_t expected_prev =
            microide::editor::TextLayout::PreviousTextColumn(whole, expected_clamped);
        Expect(at.prev_column == expected_prev,
               "prev_column must match PreviousTextColumn over the whole line" + where);
        Expect(at.prev_char == whole[expected_prev],
               "prev_char must be the byte at prev_column" + where);
      }
    }
  }
}

void TestTextBufferOperatorIndexAliasesTheTreesMaterializedLine() {
  microide::editor::TextBuffer buffer;
  buffer.ResetFromText("abcdefghij\nplain second line\n");
  buffer.ReplaceTextRange(0, 4, 0, 4, "WXYZ");

  const std::string_view view = buffer.LineView(0);
  const std::string& ref = buffer[0];
  Expect(ref == "abcdWXYZefghij", "the aliased reference still reads the right bytes");
  Expect(ref.data() == view.data(),
         "operator[] on a spanning line must alias the tree's copy, not make a second one");
  Expect(buffer.materialized_line_count() == 0,
         "aliasing must not add an entry to the compatibility line cache");

  const std::string& contiguous = buffer[1];
  Expect(contiguous == "plain second line", "the untouched line reads correctly");
  Expect(contiguous.data() != buffer.LineView(1).data(),
         "a contiguous line is served from the compatibility cache, not the tree");
  Expect(buffer.materialized_line_count() == 1, "the contiguous read materialized one line");

  buffer.ReplaceTextRange(1, 0, 1, 0, "x");
  Expect(buffer.materialized_line_count() == 0, "a mutation clears the compatibility cache");
  Expect(buffer[0] == "abcdWXYZefghij", "content survives the mutation on the other line");
}

void TestPieceTreeLiveDocumentByteCeiling() {
  PieceTree tree({"hello", "world"});
  const std::vector<std::string> before = tree.ToVector();
  const std::size_t before_bytes = tree.ByteSize();
  const std::size_t before_lines = tree.LineCount();

  tree.SetMaxLiveDocumentBytesForTesting(static_cast<std::uint32_t>(before_bytes + 2));

  tree.PushBackLine("x");
  Expect(!tree.LastMutationRejectedForByteCeiling(), "an in-budget mutation applies");
  Expect(tree.LineCount() == before_lines + 1, "the in-budget mutation grew the document");

  const std::vector<std::string> at_cap = tree.ToVector();
  const std::size_t at_cap_bytes = tree.ByteSize();
  tree.PushBackLine("this would overflow the lowered ceiling");
  Expect(tree.LastMutationRejectedForByteCeiling(),
         "a mutation past the byte ceiling is rejected");
  Expect(tree.ToVector() == at_cap, "a rejected mutation leaves content unchanged");
  Expect(tree.ByteSize() == at_cap_bytes, "a rejected mutation leaves byte size unchanged");

  tree.EraseLine(0);
  Expect(!tree.LastMutationRejectedForByteCeiling(), "a shrinking mutation is never refused");
  Expect(tree.LineCount() == at_cap.size() - 1, "the delete applied");
}

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

  tree.InsertTextForTesting(2, "XYZ");
  tree.InsertTextForTesting(15, "-mid-");
  tree.InsertLine(1, "inserted");
  out.clear();
  tree.AppendWholeText(out);
  Expect(out == join(tree), "after mid-line edits: whole text still matches the line join");
  Expect(out.size() == tree.ByteSize(), "whole text length still equals ByteSize()");

  std::string prefixed = "PREFIX:";
  tree.AppendWholeText(prefixed);
  Expect(prefixed == "PREFIX:" + out, "AppendWholeText appends, it does not overwrite");
}

void TestPieceTreeReplaceTextRangeMatchesLineForm() {
  const auto splice = [](std::vector<std::string> lines, std::size_t sl, std::size_t sc,
                         std::size_t el, std::size_t ec, std::string_view text) {
    sl = std::min(sl, lines.size() - 1);
    el = std::clamp(el, sl, lines.size() - 1);
    sc = std::min(sc, lines[sl].size());
    ec = std::min(ec, lines[el].size());
    std::string joined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (i != 0) joined.push_back('\n');
      joined.append(lines[i]);
    }
    std::size_t start_base = 0;
    for (std::size_t i = 0; i < sl; ++i) start_base += lines[i].size() + 1;
    std::size_t end_base = 0;
    for (std::size_t i = 0; i < el; ++i) end_base += lines[i].size() + 1;
    const std::size_t start = start_base + sc;
    const std::size_t end = std::max(start, end_base + ec);
    joined.replace(start, end - start, text);
    std::vector<std::string> out;
    std::size_t from = 0;
    for (std::size_t i = 0; i <= joined.size(); ++i) {
      if (i == joined.size() || joined[i] == '\n') {
        out.emplace_back(joined.substr(from, i - from));
        from = i + 1;
      }
    }
    return out;
  };

  struct Case {
    std::vector<std::string> lines;
    std::size_t start_line;
    std::size_t start_column;
    std::size_t end_line;
    std::size_t end_column;
    std::string text;
  };
  const std::vector<Case> cases = {
      {{"alpha", "bravo", "charlie"}, 1, 2, 1, 2, "X"},
      {{"alpha", "bravo", "charlie"}, 1, 2, 1, 4, ""},
      {{"alpha", "bravo", "charlie"}, 1, 2, 1, 4, "ZZZZ"},
      {{"alpha", "bravo", "charlie"}, 0, 5, 1, 0, ""},
      {{"alpha", "bravo", "charlie"}, 1, 3, 1, 3, "\n"},
      {{"alpha", "bravo", "charlie"}, 0, 1, 2, 3, "mid"},
      {{"alpha", "bravo", "charlie"}, 0, 0, 2, 7, ""},
      {{"alpha", "bravo", "charlie"}, 2, 7, 2, 7, "\ntail"},
      {{"alpha"}, 0, 0, 0, 0, "a\nb\nc"},
      {{""}, 0, 0, 0, 0, "seed"},
      {{"alpha", "bravo"}, 1, 99, 1, 99, "!"},
      {{"alpha", "bravo"}, 9, 0, 9, 0, "!"},
      {{"alpha", "bravo"}, 1, 4, 1, 1, "swapped"},
  };

  for (const Case& c : cases) {
    PieceTree tree(c.lines);
    tree.ReplaceTextRange(c.start_line, c.start_column, c.end_line, c.end_column, c.text);
    const std::vector<std::string> expected = splice(c.lines, c.start_line, c.start_column,
                                                     c.end_line, c.end_column, c.text);
    Expect(tree.ToVector() == expected, "ReplaceTextRange must match the joined-string oracle");
    Expect(tree.LineCount() == expected.size(),
           "ReplaceTextRange must keep the line count in sync with the content");
    for (std::size_t i = 0; i < expected.size(); ++i) {
      Expect(tree.LineView(i) == expected[i], "ReplaceTextRange line content");
      Expect(tree.LineLength(i) == expected[i].size(), "ReplaceTextRange line length");
    }
  }
}

void TestPieceTreeReplaceTextRangeRandomizedEquivalence() {
  const std::uint64_t seeds[] = {11u, 97u, 0xFACEu, 0x5EEDu};
  for (std::uint64_t seed : seeds) {
    Rng rng(seed);
    PieceTree tree;
    VectorModel model;
    std::vector<std::string> initial;
    const std::size_t initial_lines = 1 + rng.Below(20);
    for (std::size_t i = 0; i < initial_lines; ++i) initial.push_back(MakeLine(rng));
    tree.Reset(initial);
    model.Reset(initial);

    for (int step = 0; step < 400; ++step) {
      const std::size_t n = model.lines.size();
      if (n == 0) {
        tree.ReplaceLineRange(0, 0, {""});
        model.ReplaceLineRange(0, 0, {""});
        continue;
      }
      if ((rng.Next() & 3u) == 0u) {
        const std::size_t start = rng.Below(n + 1);
        const std::size_t removed = rng.Below((n - std::min(start, n)) + 1);
        std::vector<std::string> inserted;
        for (std::size_t i = 0; i < rng.Below(3); ++i) inserted.push_back(MakeLine(rng));
        tree.ReplaceLineRange(start, removed, inserted);
        model.ReplaceLineRange(start, removed, inserted);
        ExpectEquivalent(tree, model, "after random line replace");
        continue;
      }

      const std::size_t start_line = rng.Below(n);
      const std::size_t start_column = rng.Below(model.lines[start_line].size() + 1);
      const std::size_t end_line = start_line + rng.Below(n - start_line);
      const std::size_t end_column = rng.Below(model.lines[end_line].size() + 1);
      std::string text;
      for (std::size_t i = 0, count = rng.Below(6); i < count; ++i) {
        text.push_back((rng.Below(8) == 0) ? '\n'
                                           : static_cast<char>('a' + static_cast<char>(rng.Below(26))));
      }

      std::string joined;
      for (std::size_t i = 0; i < model.lines.size(); ++i) {
        if (i != 0) joined.push_back('\n');
        joined.append(model.lines[i]);
      }
      std::size_t start_base = 0;
      for (std::size_t i = 0; i < start_line; ++i) start_base += model.lines[i].size() + 1;
      std::size_t end_base = 0;
      for (std::size_t i = 0; i < end_line; ++i) end_base += model.lines[i].size() + 1;
      const std::size_t from = start_base + start_column;
      const std::size_t to = std::max(from, end_base + end_column);

      std::string extracted = "KEEP:";
      tree.AppendTextRange(start_line, start_column, end_line, end_column, extracted);
      Expect(extracted == "KEEP:" + joined.substr(from, to - from),
             "AppendTextRange must append exactly the span's bytes");

      joined.replace(from, to - from, text);
      std::vector<std::string> expected;
      std::size_t cursor = 0;
      for (std::size_t i = 0; i <= joined.size(); ++i) {
        if (i == joined.size() || joined[i] == '\n') {
          expected.emplace_back(joined.substr(cursor, i - cursor));
          cursor = i + 1;
        }
      }

      tree.ReplaceTextRange(start_line, start_column, end_line, end_column, text);
      model.lines = expected;
      ExpectEquivalent(tree, model, "after random text-range replace");
    }
  }
}

void TestPieceTreeReplaceTextRangeHonorsByteCeiling() {
  PieceTree tree({"hello", "world"});
  const std::vector<std::string> before = tree.ToVector();
  tree.SetMaxLiveDocumentBytesForTesting(static_cast<std::uint32_t>(tree.ByteSize()));

  tree.ReplaceTextRange(0, 5, 0, 5, "overflow");
  Expect(tree.LastMutationRejectedForByteCeiling(),
         "a column-scoped splice past the byte ceiling must be rejected");
  Expect(tree.ToVector() == before, "a rejected column-scoped splice changes nothing");

  tree.ReplaceTextRange(0, 0, 0, 5, "HI");
  Expect(!tree.LastMutationRejectedForByteCeiling(), "a shrinking splice is never refused");
  Expect(tree.LineView(0) == "HI", "the shrinking splice applied");
}

}

void TestPieceTreeLineStartMemoInvalidation() {
  const std::vector<std::string> base = {"alpha", "bravo", "charlie", "delta", "echo"};

  {
    PieceTree tree(base);
    (void)tree.LineView(1);
    tree.InsertLine(0, "prefix-line");
    Expect(tree.LineView(2) == "bravo",
           "an insert before the memoized line must invalidate its cached start");
  }

  {
    PieceTree tree(base);
    (void)tree.LineView(2);
    tree.EraseLine(0);
    Expect(tree.LineView(3) == "echo",
           "an erase before the memoized line must invalidate its cached start");
  }

  {
    PieceTree tree(base);
    (void)tree.LineView(1);
    tree.SetLine(0, "a-much-longer-first-line");
    Expect(tree.LineView(2) == "charlie",
           "a length-changing edit must invalidate the cached start after it");
  }

  {
    PieceTree tree(base);
    (void)tree.LineLength(1);
    tree.InsertLine(0, "prefix-line");
    Expect(tree.LineLength(2) == 5, "LineLength must not read a stale cached start");
  }

  {
    PieceTree tree(base);
    (void)tree.LineView(1);
    tree.Reset({"x", "much longer second line here", "z"});
    Expect(tree.LineView(2) == "z", "Reset must invalidate the line-start memo");
  }
  {
    PieceTree tree(base);
    (void)tree.LineView(1);
    tree.ResetFromText("one\ntwo-is-much-longer\nthree");
    Expect(tree.LineView(2) == "three", "ResetFromText must invalidate the line-start memo");
  }

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

void TestLineBlobPrependMatchesTheGeneralSplice() {
  std::mt19937 rng(20260920u);
  std::uniform_int_distribution<int> line_count(0, 12);
  std::uniform_int_distribution<int> line_length(0, 40);
  std::uniform_int_distribution<int> byte(97, 122);
  const auto make_blob = [&]() {
    editor::LineBlob blob;
    const int lines = line_count(rng);
    for (int i = 0; i < lines; ++i) {
      std::string line(static_cast<std::size_t>(line_length(rng)), 'x');
      for (char& c : line) {
        c = static_cast<char>(byte(rng));
      }
      blob.push_back(line);
    }
    return blob;
  };
  const auto lines_of = [](const editor::LineBlob& blob) {
    std::vector<std::string> out;
    out.reserve(blob.size());
    for (std::size_t i = 0; i < blob.size(); ++i) {
      out.emplace_back(blob[i]);
    }
    return out;
  };

  for (int trial = 0; trial < 400; ++trial) {
    const editor::LineBlob base = make_blob();
    const editor::LineBlob head = make_blob();

    editor::LineBlob actual = base;
    actual.prepend(head);

    editor::LineBlob expected = base;
    expected.replace_range(0, 0, head);

    Expect(actual.size() == expected.size(),
           "prepend must produce the same line count as the splice it replaced");
    Expect(lines_of(actual) == lines_of(expected),
           "prepend must produce the same lines, byte for byte, as the splice it replaced");
    Expect(actual.size() == base.size() + head.size(),
           "prepend must keep every line of both sides");
  }

  editor::LineBlob grown;
  std::vector<std::string> model;
  for (int i = 0; i < 1000; ++i) {
    const std::string line = "line " + std::to_string(999 - i);
    editor::LineBlob one;
    one.push_back(line);
    grown.prepend(one);
    model.insert(model.begin(), line);
  }
  Expect(lines_of(grown) == model,
         "a thousand one-line prepends must build the same blob as a front-inserting model");

  editor::LineBlob self = editor::LineBlob::Of({"a", "bb", "ccc"});
  self.prepend(self);
  Expect(lines_of(self) == std::vector<std::string>({"a", "bb", "ccc", "a", "bb", "ccc"}),
         "prepending a blob to itself must double it, not read shifted offsets");
}

void TestPieceTreeSnapshotReadsAsTheDocumentDidWhenTaken() {
  std::mt19937 rng(20261106);
  PieceTree tree(std::vector<std::string>{"alpha", "beta", "", "gamma"});
  VectorModel model;
  model.Reset({"alpha", "beta", "", "gamma"});
  std::vector<std::pair<editor::DocumentSnapshot, std::vector<std::string>>> taken;
  for (int round = 0; round < 400; ++round) {
    const std::size_t line = rng() % (model.lines.size() + 1);
    std::vector<std::string> inserted;
    if (rng() % 3 != 0) {
      inserted.push_back("t" + std::to_string(round));
    }
    const std::size_t removed = rng() % 2;
    tree.ReplaceLineRange(line, removed, inserted);
    model.ReplaceLineRange(line, removed, inserted);
    if (round % 7 == 0) {
      taken.emplace_back(tree.Snapshot(), model.lines);
    }
    if (round == 200) {
      tree.CompactAddBufferForTesting();
    }
  }
  PieceTree copy = tree;
  copy.InsertLine(0, "from the copy");
  tree.InsertLine(0, "from the tree");
  Expect(copy.LineView(0) == "from the copy" && tree.LineView(0) == "from the tree",
         "a copy and its source never append into the same chunk");
  tree.Reset({"reset"});
  for (const auto& [snapshot, lines] : taken) {
    Expect(snapshot.LineCount() == lines.size() && snapshot.ToLines() == lines,
           "every snapshot still reads as the document did when it was taken");
  }
  Expect(PieceTree().Snapshot().ToLines() == PieceTree().ToVector() && PieceTree().Snapshot().empty(),
         "the empty tree has no lines, as ToVector says");
  PieceTree one_empty(std::vector<std::string>{""});
  Expect(one_empty.Snapshot().ToLines() == std::vector<std::string>{""},
         "one empty line is one empty line");
  PieceTree text;
  text.ResetFromText("a\n\nb\n");
  std::string joined;
  text.Snapshot().AppendText(joined);
  Expect(text.Snapshot().ToLines() == text.ToVector() && joined == "a\n\nb\n",
         "a trailing newline is a last empty line, and the text round-trips");
}

void TestPieceTreeSnapshotIsReadableWhileTheTreeIsEdited() {
  PieceTree tree(std::vector<std::string>{"start"});
  for (int i = 0; i < 50; ++i) {
    tree.InsertTextForTesting(static_cast<std::uint32_t>(tree.ByteSize()), "x");
  }
  const editor::DocumentSnapshot snapshot = tree.Snapshot();
  const std::vector<std::string> expected = tree.ToVector();
  std::atomic<bool> same{true};
  std::thread reader([&] {
    for (int pass = 0; pass < 200; ++pass) {
      std::string text;
      snapshot.AppendText(text);
      if (snapshot.ToLines() != expected) {
        same = false;
      }
    }
  });
  for (int i = 0; i < 2000; ++i) {
    tree.InsertTextForTesting(static_cast<std::uint32_t>(tree.ByteSize()), "y");
  }
  reader.join();
  Expect(same.load(), "the snapshot read the same text on every pass");
}

void TestPieceTreeStaysShallowUnderAnEditAtEveryLine() {
  constexpr std::size_t kLines = 4000;
  std::string text;
  for (std::size_t i = 0; i < kLines; ++i) {
    text += "line " + std::to_string(i) + " the quick brown fox\n";
  }
  const auto expect_shallow = [](const PieceTree& tree, const std::string& what) {
    Expect(tree.DepthForTesting() <= 64,
           what + ": the tree stays shallow, depth " + std::to_string(tree.DepthForTesting()));
  };
  {
    PieceTree tree;
    tree.ResetFromText(text);
    for (std::size_t i = kLines; i-- > 0;) {
      tree.ReplaceTextRange(i, 2, i, 3, "");
    }
    expect_shallow(tree, "a delete on every line, last line first");
    Expect(tree.LineView(0) == "lie 0 the quick brown fox" &&
               tree.LineView(kLines - 1) == "lie " + std::to_string(kLines - 1) + " the quick brown fox",
           "each line lost its one byte");
  }
  {
    PieceTree tree;
    tree.ResetFromText(text);
    for (std::size_t i = 0; i < kLines; ++i) {
      tree.ReplaceTextRange(i, 2, i, 3, "");
    }
    expect_shallow(tree, "a delete on every line, first line first");
  }
  {
    PieceTree tree;
    tree.ResetFromText(text);
    for (std::size_t i = kLines; i-- > 0;) {
      tree.ReplaceTextRange(i, 4, i, 4, "x");
    }
    expect_shallow(tree, "an insert on every line, last line first");
    Expect(tree.LineView(7) == "linex 7 the quick brown fox", "each line took its byte");
  }
}

void RegisterPieceTreeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PieceTree/StaysShallowUnderAnEditAtEveryLine", TestPieceTreeStaysShallowUnderAnEditAtEveryLine);
  AddTest(tests, "PieceTree/LineStartMemoInvalidation", TestPieceTreeLineStartMemoInvalidation);
  AddTest(tests, "PieceTree/LiveDocumentByteCeiling", TestPieceTreeLiveDocumentByteCeiling);
  AddTest(tests, "PieceTree/BasicSemantics", TestPieceTreeBasicSemantics);
  AddTest(tests, "PieceTree/SnapshotReadsAsTheDocumentDidWhenTaken",
          TestPieceTreeSnapshotReadsAsTheDocumentDidWhenTaken);
  AddTest(tests, "PieceTree/SnapshotIsReadableWhileTheTreeIsEdited",
          TestPieceTreeSnapshotIsReadableWhileTheTreeIsEdited);
  AddTest(tests, "PieceTree/AddBufferCompaction", TestPieceTreeAddBufferCompaction);
  AddTest(tests, "PieceTree/AddBufferStaysBoundedUnderRepeatedLargeEdits",
          TestPieceTreeAddBufferStaysBoundedUnderRepeatedLargeEdits);
  AddTest(tests, "PieceTree/AddChunksNeverMoveStoredHistory",
          TestPieceTreeAddChunksNeverMoveStoredHistory);
  AddTest(tests, "PieceTree/AddChunkHoldsAnOversizedInsertContiguously",
          TestPieceTreeAddChunkHoldsAnOversizedInsertContiguously);
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
  AddTest(tests, "PieceTree/TextBufferOperatorIndexAliasesTheTreesMaterializedLine",
          TestTextBufferOperatorIndexAliasesTheTreesMaterializedLine);
  AddTest(tests, "PieceTree/CaretNeighborhoodMatchesWholeLineReading",
          TestCaretNeighborhoodMatchesWholeLineReading);
  AddTest(tests, "PieceTree/LineWindowMatchesSubstring", TestPieceTreeLineWindowMatchesSubstring);
  AddTest(tests, "PieceTree/LineWindowDoesNotMaterializeTheLine",
          TestPieceTreeLineWindowDoesNotMaterializeTheLine);
  AddTest(tests, "PieceTree/ReplaceTextRangeMatchesLineForm",
          TestPieceTreeReplaceTextRangeMatchesLineForm);
  AddTest(tests, "PieceTree/ReplaceTextRangeRandomizedEquivalence",
          TestPieceTreeReplaceTextRangeRandomizedEquivalence);
  AddTest(tests, "PieceTree/ReplaceTextRangeHonorsByteCeiling",
          TestPieceTreeReplaceTextRangeHonorsByteCeiling);
  AddTest(tests, "PieceTree/ExtractIntoBlobSizesItsByteBuffer",
          TestPieceTreeExtractIntoBlobSizesItsByteBuffer);
  AddTest(tests, "PieceTree/SmallBlobIsAllocationFree", TestPieceTreeSmallBlobIsAllocationFree);
  AddTest(tests, "PieceTree/LineBlobPrependMatchesTheGeneralSplice",
          TestLineBlobPrependMatchesTheGeneralSplice);
}

}
