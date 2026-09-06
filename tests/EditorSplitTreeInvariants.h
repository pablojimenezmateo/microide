#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "workspace/EditorSplitTree.h"

namespace microide::tests {

// The structural contract every consumer of the tree relies on -- the pane-rect
// walk, the focus index, the per-group caches, session persistence -- stated as
// one check and run after every edit of a random edit sequence. A tree that
// breaks any of these draws a group nowhere, or two groups in one place.
inline void ExpectSplitTreeWellFormed(const microide::workspace::EditorSplitTree& tree,
                                      const std::string& context) {
  const auto nodes = tree.nodes();
  Expect(tree.node_count() <= microide::workspace::EditorSplitTree::kMaxNodes, "node budget: " + context);
  Expect(tree.root() < tree.node_count(), "root is a live node: " + context);
  Expect(nodes[tree.root()].parent == microide::workspace::EditorSplitTree::kNoNode, "root has no parent: " + context);

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
  Expect(leaves >= 1 && leaves <= microide::workspace::kMaxEditorGroups, "leaf count within the cap: " + context);

  // Ordinal <-> node is a bijection over the leaves.
  std::vector<bool> leaf_seen(tree.node_count(), false);
  for (std::size_t ordinal = 0; ordinal < tree.leaf_count(); ++ordinal) {
    const std::uint8_t node = tree.NodeForLeaf(ordinal);
    Expect(node != microide::workspace::EditorSplitTree::kNoNode && node < tree.node_count() && nodes[node].leaf(),
           "every ordinal names a live leaf: " + context);
    if (node == microide::workspace::EditorSplitTree::kNoNode || node >= tree.node_count()) {
      return;
    }
    Expect(!leaf_seen[node], "ordinals name distinct leaves: " + context);
    leaf_seen[node] = true;
    Expect(tree.LeafOrdinal(node) == ordinal, "LeafOrdinal inverts NodeForLeaf: " + context);
  }
  Expect(tree.NodeForLeaf(tree.leaf_count()) == microide::workspace::EditorSplitTree::kNoNode,
         "one past the last ordinal is no leaf: " + context);

  // The persisted form round-trips to an equal, well-formed tree.
  microide::workspace::EditorSplitTree reloaded;
  Expect(reloaded.Load(tree.Flatten()), "Flatten() loads back: " + context);
  Expect(reloaded == tree, "the reloaded tree equals the original: " + context);
  Expect(reloaded.leaf_count() == tree.leaf_count(), "and keeps the leaf count: " + context);
}

}  // namespace microide::tests
