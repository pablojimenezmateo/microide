#include "TestSupport.h"

#include "workspace/EditorSplitTree.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::EditorSplitOrientation;
using microide::workspace::EditorSplitTree;
using microide::workspace::kMaxEditorGroups;

// The structural contract every consumer of the tree relies on -- the pane-rect
// walk, the focus index, the per-group caches, session persistence -- stated as
// one check and run after every edit of a random edit sequence. A tree that
// breaks any of these draws a group nowhere, or two groups in one place.
void ExpectWellFormed(const EditorSplitTree& tree, const std::string& context) {
  const auto nodes = tree.nodes();
  Expect(tree.node_count() <= EditorSplitTree::kMaxNodes, "node budget: " + context);
  Expect(tree.root() < tree.node_count(), "root is a live node: " + context);
  Expect(nodes[tree.root()].parent == EditorSplitTree::kNoNode, "root has no parent: " + context);

  // Reachability and per-branch shape, by a walk of our own.
  std::vector<bool> seen(tree.node_count(), false);
  std::vector<std::uint8_t> stack{tree.root()};
  std::size_t leaves = 0;
  while (!stack.empty()) {
    const std::uint8_t index = stack.back();
    stack.pop_back();
    Expect(index < tree.node_count(), "child index in range: " + context);
    if (index >= tree.node_count()) {
      return;
    }
    Expect(!seen[index], "a node is reached once: " + context);
    seen[index] = true;
    const auto& node = nodes[index];
    if (node.leaf()) {
      Expect(node.children.empty() && node.weights.empty(), "a leaf has no children: " + context);
      ++leaves;
      continue;
    }
    Expect(node.children.size() >= 2, "a branch splits at least two ways: " + context);
    Expect(node.children.size() == node.weights.size(), "one weight per child: " + context);
    float total = 0.0f;
    for (std::size_t i = 0; i < node.children.size(); ++i) {
      const std::uint8_t child = node.children[i];
      Expect(child < tree.node_count(), "child in range: " + context);
      if (child >= tree.node_count()) {
        return;
      }
      Expect(nodes[child].parent == index, "a child points back at its branch: " + context);
      Expect(!nodes[child].leaf() ? nodes[child].orientation != node.orientation : true,
             "no branch nests in a same-axis branch (canonical form): " + context);
      Expect(std::isfinite(node.weights[i]) && node.weights[i] > 0.0f,
             "a weight is a positive finite share: " + context);
      total += node.weights[i];
      stack.push_back(child);
    }
    Expect(std::abs(total - 1.0f) < 1e-4f, "weights sum to one: " + context);
  }
  for (std::size_t i = 0; i < seen.size(); ++i) {
    Expect(seen[i], "every stored node is reachable: " + context);
  }
  Expect(leaves == tree.leaf_count(), "leaf_count matches the walk: " + context);
  Expect(leaves >= 1 && leaves <= kMaxEditorGroups, "leaf count within the cap: " + context);

  // Ordinal <-> node is a bijection over the leaves.
  std::vector<bool> leaf_seen(tree.node_count(), false);
  for (std::size_t ordinal = 0; ordinal < tree.leaf_count(); ++ordinal) {
    const std::uint8_t node = tree.NodeForLeaf(ordinal);
    Expect(node != EditorSplitTree::kNoNode && node < tree.node_count() && nodes[node].leaf(),
           "every ordinal names a live leaf: " + context);
    if (node == EditorSplitTree::kNoNode || node >= tree.node_count()) {
      return;
    }
    Expect(!leaf_seen[node], "ordinals name distinct leaves: " + context);
    leaf_seen[node] = true;
    Expect(tree.LeafOrdinal(node) == ordinal, "LeafOrdinal inverts NodeForLeaf: " + context);
  }
  Expect(tree.NodeForLeaf(tree.leaf_count()) == EditorSplitTree::kNoNode,
         "one past the last ordinal is no leaf: " + context);

  // The persisted form round-trips to an equal, well-formed tree.
  EditorSplitTree reloaded;
  Expect(reloaded.Load(tree.Flatten()), "Flatten() loads back: " + context);
  Expect(reloaded == tree, "the reloaded tree equals the original: " + context);
  Expect(reloaded.leaf_count() == tree.leaf_count(), "and keeps the leaf count: " + context);
}

void TestEditorSplitTreeRandomEditsKeepItWellFormed() {
  std::uint64_t state = 0x2545F4914F6CDD1Dull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  const auto orientation = [&] {
    return next(2) == 0 ? EditorSplitOrientation::Vertical : EditorSplitOrientation::Horizontal;
  };

  EditorSplitTree tree;
  ExpectWellFormed(tree, "fresh");
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
        Expect(fresh == EditorSplitTree::kNoLeaf, "a full grid refuses a split: " + context);
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
    ExpectWellFormed(tree, context);
  }
}

// The two reject paths of Load: a malformed record leaves a single leaf behind
// (so a corrupt session can never produce a layout with no pane for a group),
// and a well-formed one is taken as-is.
void TestEditorSplitTreeLoadRejectsMalformedRecords() {
  using Record = microide::workspace::EditorSplitTreeRecord;
  using Entry = microide::workspace::EditorSplitNodeRecord;
  const auto branch = [](EditorSplitOrientation o, std::initializer_list<float> weights) {
    Entry entry;
    entry.orientation = o;
    for (float w : weights) {
      entry.weights.push_back(w);
    }
    return entry;
  };
  const Entry leaf;

  EditorSplitTree tree;
  Record one_child;
  one_child.push_back(branch(EditorSplitOrientation::Vertical, {1.0f}));
  one_child.push_back(leaf);
  Expect(!tree.Load(one_child) && tree.leaf_count() == 1, "a one-child branch is rejected");

  Record truncated;
  truncated.push_back(branch(EditorSplitOrientation::Vertical, {0.5f, 0.5f}));
  truncated.push_back(leaf);
  Expect(!tree.Load(truncated) && tree.leaf_count() == 1, "a truncated stream is rejected");

  Record trailing;
  trailing.push_back(leaf);
  trailing.push_back(leaf);
  Expect(!tree.Load(trailing) && tree.leaf_count() == 1, "trailing junk is rejected");

  Record good;
  good.push_back(branch(EditorSplitOrientation::Vertical, {2.0f, 1.0f, 1.0f}));
  good.push_back(leaf);
  good.push_back(branch(EditorSplitOrientation::Horizontal, {0.5f, 0.5f}));
  good.push_back(leaf);
  good.push_back(leaf);
  good.push_back(leaf);
  Expect(tree.Load(good) && tree.leaf_count() == 4, "a well-formed record loads");
  ExpectWellFormed(tree, "loaded");
  Expect(std::abs(tree.node(0).weights[0] - 0.5f) < 1e-5f, "weights are normalised on load");
}

}  // namespace

void RegisterEditorSplitTreeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorSplitTree/RandomEditsKeepItWellFormed",
          TestEditorSplitTreeRandomEditsKeepItWellFormed);
  AddTest(tests, "EditorSplitTree/LoadRejectsMalformedRecords",
          TestEditorSplitTreeLoadRejectsMalformedRecords);
}

}  // namespace microide::tests
