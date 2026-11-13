#include "TestSupport.h"

#include "workspace/SplitTree.h"
#include "SplitTreeInvariants.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::SplitOrientation;
using microide::workspace::SplitTree;
using microide::workspace::kMaxEditorGroups;

void TestEditorSplitTreeRandomEditsKeepItWellFormed() {
  std::uint64_t state = 0x2545F4914F6CDD1Dull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  const auto orientation = [&] {
    return next(2) == 0 ? SplitOrientation::Vertical : SplitOrientation::Horizontal;
  };

  SplitTree tree;
  ExpectSplitTreeWellFormed(tree, "fresh");
  for (int step = 0; step < 3000; ++step) {
    const std::string context = "step " + std::to_string(step);
    const std::size_t kind = next(10);
    if (kind < 4) {
      const std::size_t leaf = next(tree.leaf_count());
      const bool before = next(2) == 0;
      const auto record_before = tree.Flatten();
      const bool was_full = tree.full();
      const std::size_t fresh = tree.InsertLeaf(leaf, orientation(), before);
      if (was_full) {
        Expect(fresh == SplitTree::kNoLeaf, "a full grid refuses a split: " + context);
        Expect(tree.Flatten() == record_before, "and is left untouched: " + context);
      } else {
        // The new pane sits directly before or after the one that was split, so
        // the caller's parallel group vector inserts at exactly that index.
        Expect(fresh == (before ? leaf : leaf + 1),
               "the new pane's ordinal is next to the split pane: " + context);
      }
    } else if (kind < 7) {
      const std::size_t count = tree.leaf_count();
      tree.RemoveLeaf(next(count));
      Expect(tree.leaf_count() == (count > 1 ? count - 1 : 1),
             "removing a pane drops the count by one (never below one): " + context);
    } else if (kind < 9) {
      if (tree.leaf_count() >= 2) {
        const std::size_t from = next(tree.leaf_count());
        std::size_t target = next(tree.leaf_count());
        if (target == from) {
          target = (target + 1) % tree.leaf_count();
        }
        const bool before = next(2) == 0;
        const std::size_t count = tree.leaf_count();
        const std::size_t moved = tree.MoveLeaf(from, target, orientation(), before);
        const std::size_t adjusted = target > from ? target - 1 : target;
        Expect(moved == (before ? adjusted : adjusted + 1),
               "a moved pane lands next to its target in the new numbering: " + context);
        Expect(tree.leaf_count() == count, "a move keeps the pane count: " + context);
      }
    } else {
      // Drag a divider on some branch.
      for (std::uint8_t node = 0; node < tree.node_count(); ++node) {
        if (tree.node(node).leaf()) {
          continue;
        }
        const std::size_t boundary = next(tree.node(node).children.size() - 1);
        const float share = static_cast<float>(next(1000)) / 500.0f - 0.5f;  // [-0.5, 1.5)
        Expect(tree.ResizeDivider(node, boundary, share), "a live divider resizes: " + context);
        break;
      }
    }
    ExpectSplitTreeWellFormed(tree, context);
  }
}

// The two reject paths of Load: a malformed record leaves a single leaf behind
// (so a corrupt session can never produce a layout with no pane for a group),
// and a well-formed one is taken as-is.
void TestEditorSplitTreeLoadRejectsMalformedRecords() {
  using Record = microide::workspace::SplitTreeRecord;
  using Entry = microide::workspace::SplitNodeRecord;
  const auto branch = [](SplitOrientation o, std::initializer_list<float> weights) {
    Entry entry;
    entry.orientation = o;
    for (float w : weights) {
      entry.weights.push_back(w);
    }
    return entry;
  };
  const Entry leaf;

  SplitTree tree;
  Record one_child;
  one_child.push_back(branch(SplitOrientation::Vertical, {1.0f}));
  one_child.push_back(leaf);
  Expect(!tree.Load(one_child) && tree.leaf_count() == 1, "a one-child branch is rejected");

  Record truncated;
  truncated.push_back(branch(SplitOrientation::Vertical, {0.5f, 0.5f}));
  truncated.push_back(leaf);
  Expect(!tree.Load(truncated) && tree.leaf_count() == 1, "a truncated stream is rejected");

  Record trailing;
  trailing.push_back(leaf);
  trailing.push_back(leaf);
  Expect(!tree.Load(trailing) && tree.leaf_count() == 1, "trailing junk is rejected");

  Record good;
  good.push_back(branch(SplitOrientation::Vertical, {2.0f, 1.0f, 1.0f}));
  good.push_back(leaf);
  good.push_back(branch(SplitOrientation::Horizontal, {0.5f, 0.5f}));
  good.push_back(leaf);
  good.push_back(leaf);
  good.push_back(leaf);
  Expect(tree.Load(good) && tree.leaf_count() == 4, "a well-formed record loads");
  ExpectSplitTreeWellFormed(tree, "loaded");
  Expect(std::abs(tree.node(0).weights[0] - 0.5f) < 1e-5f, "weights are normalised on load");
}

// A whole tree seated beside a leaf: what a split terminal tab dropped on another
// tab's pane edge does to the target's grid.
void TestSplitTreeGraftKeepsShapeAndCanonicalForm() {
  SplitTree row;  // two panes side by side
  Expect(row.InsertLeaf(0, SplitOrientation::Vertical, false) == 1, "row of two");
  SplitTree stack;  // two panes stacked
  Expect(stack.InsertLeaf(0, SplitOrientation::Horizontal, false) == 1, "stack of two");

  // Same axis as the seat: the graft's panes join the row instead of nesting.
  SplitTree target;
  Expect(target.GraftTree(0, SplitOrientation::Vertical, false, row) == 1, "graft lands after the target");
  ExpectSplitTreeWellFormed(target, "row graft");
  Expect(target.leaf_count() == 3 && target.node(target.root()).children.size() == 3,
         "a row grafted beside a pane along the row's own axis is one flat row of three");
  Expect(std::abs(target.node(target.root()).weights[0] - 0.5f) < 1e-5f &&
             std::abs(target.node(target.root()).weights[1] - 0.25f) < 1e-5f &&
             std::abs(target.node(target.root()).weights[2] - 0.25f) < 1e-5f,
         "the target keeps its half and the graft splits the other half by its own shares");

  // Other axis: the graft keeps its branch.
  SplitTree beside;
  Expect(beside.GraftTree(0, SplitOrientation::Vertical, true, stack) == 0, "graft lands before the target");
  ExpectSplitTreeWellFormed(beside, "stack graft");
  const auto& root = beside.node(beside.root());
  Expect(beside.leaf_count() == 3 && root.orientation == SplitOrientation::Vertical &&
             root.children.size() == 2 && !beside.node(root.children[0]).leaf() &&
             beside.node(root.children[0]).orientation == SplitOrientation::Horizontal,
         "a stack grafted on the left of a pane stays a stack beside it");

  // A single leaf is a plain insert.
  SplitTree one;
  SplitTree single;
  Expect(single.GraftTree(0, SplitOrientation::Horizontal, false, one) == 1 && single.leaf_count() == 2,
         "a one-pane graft is an insert");

  // Past the cap: refused, untouched.
  SplitTree big;
  while (big.leaf_count() < kMaxEditorGroups - 1) {
    (void)big.InsertLeaf(0, SplitOrientation::Vertical, false);
  }
  const auto before = big.Flatten();
  Expect(big.GraftTree(0, SplitOrientation::Horizontal, false, row) == SplitTree::kNoLeaf &&
             big.Flatten() == before,
         "a graft past the cap is refused and changes nothing");
  Expect(big.GraftTree(0, SplitOrientation::Horizontal, false, one) != SplitTree::kNoLeaf &&
             big.leaf_count() == kMaxEditorGroups,
         "up to the cap it fits");
}

void TestSplitTreeRandomGraftsStayWellFormed() {
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  const auto orientation = [&] {
    return next(2) == 0 ? SplitOrientation::Vertical : SplitOrientation::Horizontal;
  };
  const auto random_tree = [&](std::size_t leaves) {
    SplitTree tree;
    while (tree.leaf_count() < leaves) {
      (void)tree.InsertLeaf(next(tree.leaf_count()), orientation(), next(2) == 0);
    }
    return tree;
  };
  for (int step = 0; step < 2000; ++step) {
    const std::string context = "graft step " + std::to_string(step);
    SplitTree target = random_tree(1 + next(kMaxEditorGroups));
    const SplitTree graft = random_tree(1 + next(kMaxEditorGroups));
    const std::size_t leaves = target.leaf_count();
    const std::size_t at = next(leaves);
    const bool fits = leaves + graft.leaf_count() <= kMaxEditorGroups;
    const auto record_before = target.Flatten();
    const std::size_t first = target.GraftTree(at, orientation(), next(2) == 0, graft);
    ExpectSplitTreeWellFormed(target, context);
    if (!fits) {
      Expect(first == SplitTree::kNoLeaf && target.Flatten() == record_before, context + ": refused untouched");
      continue;
    }
    Expect(first != SplitTree::kNoLeaf && first <= leaves, context + ": seated at or after the target");
    Expect(target.leaf_count() == leaves + graft.leaf_count(), context + ": every graft leaf arrived");
  }
}

}  // namespace

void RegisterSplitTreeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SplitTree/GraftKeepsShapeAndCanonicalForm", TestSplitTreeGraftKeepsShapeAndCanonicalForm);
  AddTest(tests, "SplitTree/RandomGraftsStayWellFormed", TestSplitTreeRandomGraftsStayWellFormed);
  AddTest(tests, "SplitTree/RandomEditsKeepItWellFormed",
          TestEditorSplitTreeRandomEditsKeepItWellFormed);
  AddTest(tests, "SplitTree/LoadRejectsMalformedRecords",
          TestEditorSplitTreeLoadRejectsMalformedRecords);
}

}  // namespace microide::tests
