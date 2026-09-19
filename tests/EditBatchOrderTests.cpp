// The application order for a batch of ranged edits, against an independent
// oracle.
//
// `OrderEditsForApplication` and `EditsOverlap` are what three appliers (LSP
// `TextEdit[]`, a plugin's `apply_edits`, a formatter result) share, and neither
// had a test naming it. The header records the defect that created it: one
// applier sorted on the start alone, which reverses two same-position inserts on
// every libstdc++ and is unspecified elsewhere -- a silent text-scrambling bug
// that only shows with two edits at one position.
//
// So the tie rules are what matter, and they are checked two ways: as stated
// rules, and by APPLYING the batch. The application oracle is built the other
// way round -- edits sorted ASCENDING and the result concatenated piece by piece
// -- so it shares no code with the descending apply loop it judges. Two
// same-position inserts agree between the two constructions only if the tie rule
// is right.

#include "TestSupport.h"

#include <algorithm>
#include <cstddef>
#include <random>
#include <string>
#include <vector>

#include "editor/EditBatchOrder.h"
#include "editor/EditTypes.h"

namespace microide::tests {
namespace {

using microide::editor::EditsOverlap;
using microide::editor::OrderEditsForApplication;
using microide::editor::SelectionRange;
using microide::editor::TextPosition;

SelectionRange OnLine0(std::size_t start, std::size_t end) {
  return SelectionRange{TextPosition{0, start}, TextPosition{0, end}};
}

bool Before(const TextPosition& a, const TextPosition& b) {
  return a.line < b.line || (a.line == b.line && a.column < b.column);
}

SelectionRange Normalized(SelectionRange range) {
  if (Before(range.end, range.start)) {
    std::swap(range.start, range.end);
  }
  return range;
}

// O(n^2) pairwise oracle: two ranges intersect when they share a pre-edit byte.
// Touching endpoints are not an overlap -- an insert at P beside a replace of
// [P, Q) is the shape LSP explicitly permits.
bool AnyPairIntersects(const std::vector<SelectionRange>& ranges) {
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    for (std::size_t j = i + 1; j < ranges.size(); ++j) {
      const SelectionRange a = Normalized(ranges[i]);
      const SelectionRange b = Normalized(ranges[j]);
      if (Before(a.start, b.end) && Before(b.start, a.end)) {
        return true;
      }
    }
  }
  return false;
}

// Apply a disjoint batch by CONCATENATION, ascending -- independent of the
// descending apply loop under test.
std::string ApplyAscending(const std::string& text,
                           const std::vector<SelectionRange>& ranges,
                           const std::vector<std::string>& replacements) {
  std::vector<std::size_t> ascending(ranges.size());
  for (std::size_t i = 0; i < ascending.size(); ++i) {
    ascending[i] = i;
  }
  std::stable_sort(ascending.begin(), ascending.end(), [&](std::size_t a, std::size_t b) {
    const SelectionRange ra = Normalized(ranges[a]);
    const SelectionRange rb = Normalized(ranges[b]);
    if (Before(ra.start, rb.start)) return true;
    if (Before(rb.start, ra.start)) return false;
    // Same start: the LONGER range is applied first by the code under test, so
    // in ascending order it comes last -- an insert at P lands before a replace
    // at P. Array order breaks a full tie.
    return Before(ra.end, rb.end);
  });
  std::string out;
  std::size_t cursor = 0;
  for (const std::size_t index : ascending) {
    const SelectionRange range = Normalized(ranges[index]);
    out.append(text, cursor, range.start.column - cursor);
    out.append(replacements[index]);
    cursor = range.end.column;
  }
  out.append(text, cursor, text.size() - cursor);
  return out;
}

// Apply in the order under test, highest first, splicing in place.
std::string ApplyInBatchOrder(std::string text,
                              const std::vector<SelectionRange>& ranges,
                              const std::vector<std::string>& replacements) {
  std::vector<std::size_t> order;
  OrderEditsForApplication(ranges, order);
  for (const std::size_t index : order) {
    const SelectionRange range = Normalized(ranges[index]);
    text.replace(range.start.column, range.end.column - range.start.column,
                 replacements[index]);
  }
  return text;
}

void TestOrderIsDescendingAndAPermutation() {
  std::mt19937 rng(20260923u);
  auto pick = [&](std::size_t n) {
    return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
  };
  std::size_t with_ties = 0;

  for (int iteration = 0; iteration < 3000; ++iteration) {
    std::vector<SelectionRange> ranges;
    const std::size_t count = 1 + pick(6);
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t a = pick(10);
      const std::size_t b = pick(10);
      // Deliberately including REVERSED ranges: the code normalizes them, and a
      // normalization that only happened in one of the two functions would make
      // the order and the overlap verdict disagree.
      ranges.push_back(pick(4) == 0 ? OnLine0(b, a) : OnLine0(std::min(a, b), std::max(a, b)));
    }

    std::vector<std::size_t> order;
    OrderEditsForApplication(ranges, order);

    std::vector<std::size_t> seen = order;
    std::sort(seen.begin(), seen.end());
    Expect(seen.size() == ranges.size(), "the order covers every edit");
    for (std::size_t i = 0; i < seen.size(); ++i) {
      Expect(seen[i] == i, "the order is a permutation of the input indices");
    }

    for (std::size_t i = 1; i < order.size(); ++i) {
      const SelectionRange hi = Normalized(ranges[order[i - 1]]);
      const SelectionRange lo = Normalized(ranges[order[i]]);
      Expect(!Before(hi.start, lo.start),
             "the order is descending by start, so applying one never shifts the next");
      if (hi.start.column == lo.start.column) {
        ++with_ties;
        Expect(!Before(hi.end, lo.end),
               "at one start the LONGER range applies first, or an insert at P would land "
               "inside a replace at P and be deleted with it");
        if (hi.end.column == lo.end.column) {
          Expect(order[i - 1] > order[i],
                 "at one range the LATER array entry applies first, so inserts come out in "
                 "array order");
        }
      }
    }

    Expect(EditsOverlap(ranges, order) == AnyPairIntersects(ranges),
           "EditsOverlap must agree with a pairwise intersection oracle");
  }
  Expect(with_ties > 500,
         "the sweep barely produced same-start ties, which are the whole reason this "
         "ordering exists: " + std::to_string(with_ties));
}

// Applying in the returned order must produce what an independent ascending
// concatenation produces. Non-overlapping batches only -- an overlapping batch
// is what EditsOverlap exists to refuse, and no applier runs one.
void TestApplyingInOrderMatchesAnAscendingOracle() {
  std::mt19937 rng(20260924u);
  auto pick = [&](std::size_t n) {
    return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
  };
  const std::string text = "abcdefghij";
  std::size_t applied = 0;
  std::size_t insert_pairs = 0;

  for (int iteration = 0; iteration < 4000; ++iteration) {
    std::vector<SelectionRange> ranges;
    std::vector<std::string> replacements;
    const std::size_t count = 1 + pick(4);
    // Half the batches are drawn from a SINGLE column, as zero-width inserts.
    // Uniform random ranges almost never collide -- the first version of this
    // sweep produced two same-position insert pairs in 4,000 iterations, and
    // same-position inserts are the entire reason this ordering exists. The
    // vacuity guard below is what caught that.
    const bool insert_cluster = pick(2) == 0;
    const std::size_t cluster_column = pick(text.size() + 1);
    for (std::size_t i = 0; i < count; ++i) {
      if (insert_cluster) {
        ranges.push_back(OnLine0(cluster_column, cluster_column));
      } else {
        const std::size_t a = pick(text.size() + 1);
        const std::size_t b = pick(text.size() + 1);
        ranges.push_back(OnLine0(std::min(a, b), std::max(a, b)));
      }
      replacements.push_back(std::string(1, static_cast<char>('A' + pick(4))));
    }
    std::vector<std::size_t> order;
    OrderEditsForApplication(ranges, order);
    if (EditsOverlap(ranges, order)) {
      continue;  // refused by every applier; nothing to compare
    }
    ++applied;
    for (std::size_t i = 1; i < order.size(); ++i) {
      const SelectionRange hi = Normalized(ranges[order[i - 1]]);
      const SelectionRange lo = Normalized(ranges[order[i]]);
      if (hi.start.column == lo.start.column && hi.start.column == hi.end.column &&
          lo.start.column == lo.end.column) {
        ++insert_pairs;
      }
    }

    const std::string by_order = ApplyInBatchOrder(text, ranges, replacements);
    const std::string by_oracle = ApplyAscending(text, ranges, replacements);
    Expect(by_order == by_oracle,
           "applying highest-first must equal the ascending concatenation: got \"" + by_order +
               "\", oracle \"" + by_oracle + "\"");
  }
  Expect(applied > 1000, "too few non-overlapping batches to judge: " + std::to_string(applied));
  Expect(insert_pairs > 50,
         "the sweep barely produced two inserts at one position, which is the case the "
         "unstable sort scrambled: " + std::to_string(insert_pairs));
}

// The two tie rules, stated directly rather than sampled.
void TestTieRulesAreVsCodeOrder() {
  // A replace and an insert at the same position: the replace applies first.
  const std::vector<SelectionRange> replace_then_insert = {OnLine0(2, 2), OnLine0(2, 5)};
  std::vector<std::size_t> order;
  OrderEditsForApplication(replace_then_insert, order);
  Expect(order.size() == 2 && order[0] == 1 && order[1] == 0,
         "the longer range at one start applies first, whatever the array order");

  // Three inserts at one position come out in reverse array order, so that each
  // applied insert pushes the earlier one right and the result reads left to
  // right in array order.
  const std::vector<SelectionRange> three_inserts = {OnLine0(3, 3), OnLine0(3, 3), OnLine0(3, 3)};
  OrderEditsForApplication(three_inserts, order);
  Expect(order.size() == 3 && order[0] == 2 && order[1] == 1 && order[2] == 0,
         "same-range edits apply latest-array-entry first");
  Expect(ApplyInBatchOrder("abcdef", three_inserts, {"1", "2", "3"}) == "abc123def",
         "three inserts at one position read left to right in array order");

  // Touching endpoints are not an overlap.
  const std::vector<SelectionRange> touching = {OnLine0(0, 3), OnLine0(3, 6)};
  OrderEditsForApplication(touching, order);
  Expect(!EditsOverlap(touching, order), "ranges that only touch do not overlap");
  const std::vector<SelectionRange> sharing = {OnLine0(0, 4), OnLine0(3, 6)};
  OrderEditsForApplication(sharing, order);
  Expect(EditsOverlap(sharing, order), "ranges sharing a byte do overlap");
}

}  // namespace

void RegisterEditBatchOrderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditBatchOrder/OrderIsDescendingAndAPermutation",
          TestOrderIsDescendingAndAPermutation);
  AddTest(tests, "EditBatchOrder/ApplyingInOrderMatchesAnAscendingOracle",
          TestApplyingInOrderMatchesAnAscendingOracle);
  AddTest(tests, "EditBatchOrder/TieRulesAreVsCodeOrder", TestTieRulesAreVsCodeOrder);
}

}  // namespace microide::tests
