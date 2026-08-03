#include "TestSupport.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "editor/LineEditSpan.h"

// LineEditSpan is the primitive the incremental fold caches resync against, so
// what matters is not any particular field value but the invariant the consumers
// rely on: after any batch of splices, the reported window is the ONLY place the
// stale cache and the live document may differ. These tests assert that directly
// by keeping a reference "cache" alongside a mutated "document" and checking the
// common prefix and common suffix the span claims.

namespace microide::tests {
namespace {

using microide::editor::LineEditSpan;

// Checks the two properties every consumer depends on:
//   document[0, begin)              == cache[0, begin)
//   document[current_end, doc_end)  == cache[cached_end, cache_end)
// and that the two suffixes are the same length (what lets a consumer splice).
void ExpectSpanDescribesDifference(const LineEditSpan& span,
                                   const std::vector<std::string>& cache,
                                   const std::vector<std::string>& document,
                                   const std::string& context) {
  if (span.empty()) {
    Expect(cache == document, context + ": empty span but the document changed");
    return;
  }
  const std::size_t begin = span.begin();
  const std::size_t cached_end = span.ResolvedCachedEnd(cache.size());
  const std::size_t current_end = span.ResolvedCurrentEnd(document.size());

  Expect(begin <= cached_end && begin <= current_end,
         context + ": window start ran past one of its ends");
  Expect(cache.size() - cached_end == document.size() - current_end,
         context + ": common suffixes have different lengths");

  for (std::size_t i = 0; i < begin; ++i) {
    Expect(i < cache.size() && i < document.size() && cache[i] == document[i],
           context + ": line " + std::to_string(i) + " differs inside the claimed common prefix");
  }
  const std::size_t suffix = cache.size() - cached_end;
  for (std::size_t i = 0; i < suffix; ++i) {
    Expect(cache[cached_end + i] == document[current_end + i],
           context + ": line " + std::to_string(i) + " differs inside the claimed common suffix");
  }
}

// Applies the same splice to a plain vector so the test has ground truth that
// does not share any code with the accumulator under test.
void ApplySplice(std::vector<std::string>& lines, std::size_t start, std::size_t removed,
                 const std::vector<std::string>& inserted) {
  lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(start),
              lines.begin() + static_cast<std::ptrdiff_t>(start + removed));
  lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(start), inserted.begin(),
               inserted.end());
}

std::vector<std::string> MakeDocument(std::size_t count, const std::string& tag) {
  std::vector<std::string> lines;
  lines.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    lines.push_back(tag + std::to_string(i));
  }
  return lines;
}

void TestSingleInPlaceSpliceIsExact() {
  const std::vector<std::string> cache = MakeDocument(10, "L");
  std::vector<std::string> document = cache;
  LineEditSpan span;

  ApplySplice(document, 5, 1, {"edited"});
  span.NoteSplice(5, 1, 1);

  Expect(span.begin() == 5u, "an in-place edit should anchor at the edited line");
  Expect(span.cached_end() == 6u, "an in-place edit spans exactly one cached line");
  Expect(span.current_end() == 6u, "an in-place edit spans exactly one current line");
  ExpectSpanDescribesDifference(span, cache, document, "in-place splice");
}

void TestLineCountChangingSpliceShiftsTheSuffix() {
  const std::vector<std::string> cache = MakeDocument(10, "L");
  std::vector<std::string> document = cache;
  LineEditSpan span;

  // Split line 5 into three.
  ApplySplice(document, 5, 1, {"a", "b", "c"});
  span.NoteSplice(5, 1, 3);

  Expect(span.cached_end() == 6u, "the cache-side end should stay at the pre-edit line count");
  Expect(span.current_end() == 8u, "the document-side end should follow the inserted lines");
  ExpectSpanDescribesDifference(span, cache, document, "expanding splice");
}

// The case the accumulator exists for: several edits land between two refreshes.
void TestBatchedSplicesKeepTheInvariant() {
  const std::vector<std::string> cache = MakeDocument(20, "L");
  std::vector<std::string> document = cache;
  LineEditSpan span;

  // A later edit, then an earlier one, then one that reaches past the window into
  // what was still common suffix -- the three merge cases NoteSplice distinguishes.
  ApplySplice(document, 12, 2, {"x", "y", "z"});
  span.NoteSplice(12, 2, 3);
  ApplySplice(document, 3, 1, {"p"});
  span.NoteSplice(3, 1, 1);
  ApplySplice(document, 15, 4, {});
  span.NoteSplice(15, 4, 0);

  Expect(span.begin() == 3u, "the window should start at the earliest edited line");
  ExpectSpanDescribesDifference(span, cache, document, "batched splices");
}

void TestSpliceEntirelyBeforeTheWindowStillTracksTheShift() {
  const std::vector<std::string> cache = MakeDocument(12, "L");
  std::vector<std::string> document = cache;
  LineEditSpan span;

  ApplySplice(document, 8, 1, {"tail"});
  span.NoteSplice(8, 1, 1);
  // Insert two lines well above the existing window; the window's document-side
  // end has to shift with them or the claimed suffix desynchronizes.
  ApplySplice(document, 2, 0, {"n1", "n2"});
  span.NoteSplice(2, 0, 2);

  ExpectSpanDescribesDifference(span, cache, document, "splice below the window");
}

void TestSuffixReplacedSwallowsLaterSplices() {
  const std::vector<std::string> cache = MakeDocument(10, "L");
  std::vector<std::string> document = cache;
  LineEditSpan span;

  ApplySplice(document, 4, 6, {"only"});
  span.NoteSuffixReplaced(4);
  Expect(span.cached_end() == LineEditSpan::kToEnd, "a suffix replace has no reusable cache tail");
  Expect(span.ResolvedCachedEnd(cache.size()) == cache.size(),
         "an unbounded cache end resolves to the whole cache");

  // Any further splice can only widen the window leftward.
  ApplySplice(document, 1, 1, {"q"});
  span.NoteSplice(1, 1, 1);
  Expect(span.begin() == 1u, "a later, earlier splice should still lower the window start");
  Expect(span.current_end() == LineEditSpan::kToEnd,
         "the window should stay unbounded once the suffix was replaced");
  ExpectSpanDescribesDifference(span, cache, document, "suffix replaced then edited above");
}

void TestClearedSpanReportsNoChange() {
  LineEditSpan span;
  Expect(span.empty(), "a fresh span should report no edits");
  span.NoteSplice(3, 1, 1);
  Expect(!span.empty(), "a span with a recorded splice is not empty");
  span.Clear();
  Expect(span.empty(), "Clear() should return the span to the no-edits state");
}

// Randomized cross-check. A deterministic LCG drives a few thousand splice
// batches through both the accumulator and a plain vector; any merge case the
// hand-written tests above missed shows up as a broken prefix/suffix claim.
void TestRandomizedSpliceBatchesKeepTheInvariant() {
  std::uint64_t state = 0x9E3779B97F4A7C15ULL;
  const auto next = [&state](std::uint64_t bound) -> std::size_t {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return bound == 0 ? 0 : static_cast<std::size_t>((state >> 33) % bound);
  };

  for (int trial = 0; trial < 400; ++trial) {
    const std::vector<std::string> cache = MakeDocument(30, "L");
    std::vector<std::string> document = cache;
    LineEditSpan span;

    const int splices = 1 + static_cast<int>(next(5));
    for (int s = 0; s < splices; ++s) {
      if (document.empty()) break;
      const std::size_t start = next(document.size());
      const std::size_t removed = next(document.size() - start + 1);
      const std::size_t inserted = next(4);
      std::vector<std::string> new_lines;
      for (std::size_t i = 0; i < inserted; ++i) {
        new_lines.push_back("t" + std::to_string(trial) + "_" + std::to_string(s) + "_" +
                            std::to_string(i));
      }
      ApplySplice(document, start, removed, new_lines);
      span.NoteSplice(start, removed, inserted);
    }
    ExpectSpanDescribesDifference(span, cache, document,
                                  "randomized trial " + std::to_string(trial));
  }
}

}  // namespace

void RegisterLineEditSpanTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LineEditSpan/SingleInPlaceSpliceIsExact", TestSingleInPlaceSpliceIsExact);
  AddTest(tests, "LineEditSpan/LineCountChangingSpliceShiftsTheSuffix",
          TestLineCountChangingSpliceShiftsTheSuffix);
  AddTest(tests, "LineEditSpan/BatchedSplicesKeepTheInvariant",
          TestBatchedSplicesKeepTheInvariant);
  AddTest(tests, "LineEditSpan/SpliceEntirelyBeforeTheWindowStillTracksTheShift",
          TestSpliceEntirelyBeforeTheWindowStillTracksTheShift);
  AddTest(tests, "LineEditSpan/SuffixReplacedSwallowsLaterSplices",
          TestSuffixReplacedSwallowsLaterSplices);
  AddTest(tests, "LineEditSpan/ClearedSpanReportsNoChange", TestClearedSpanReportsNoChange);
  AddTest(tests, "LineEditSpan/RandomizedSpliceBatchesKeepTheInvariant",
          TestRandomizedSpliceBatchesKeepTheInvariant);
}

}  // namespace microide::tests
