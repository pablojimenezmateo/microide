#include "workspace/EditorSplitTree.h"

#include <algorithm>
#include <cmath>

namespace microide::workspace {

namespace {

// Depth-first, children left to right: the order panes appear on screen, and so
// the order leaf ordinals (== editor group indices) are assigned in.
template <typename Visit>
void WalkDepthFirst(std::span<const EditorSplitTree::Node> nodes,
                    std::uint8_t root,
                    Visit&& visit) {
  if (root >= nodes.size()) {
    return;
  }
  util::InlineVector<std::uint8_t, EditorSplitTree::kMaxNodes> stack;
  stack.push_back(root);
  while (!stack.empty()) {
    const std::uint8_t index = stack.back();
    stack.pop_back();
    if (index >= nodes.size()) {
      continue;
    }
    if (!visit(index)) {
      return;
    }
    const EditorSplitTree::Node& current = nodes[index];
    for (std::size_t i = current.children.size(); i > 0; --i) {
      stack.push_back(current.children[i - 1]);
    }
  }
}

}  // namespace

void EditorSplitTree::Reset() {
  nodes_.clear();
  nodes_.push_back(Node{});
  root_ = 0;
  leaf_count_ = 1;
}

void EditorSplitTree::ResetToEvenSplit(std::size_t leaves, EditorSplitOrientation orientation) {
  Reset();
  const std::size_t wanted = std::clamp<std::size_t>(leaves, 1, kMaxEditorGroups);
  if (wanted < 2 || orientation == EditorSplitOrientation::None) {
    return;
  }
  nodes_[0].orientation = orientation;
  for (std::size_t i = 0; i < wanted; ++i) {
    const std::uint8_t leaf = AddNode(EditorSplitOrientation::None, 0);
    nodes_[0].children.push_back(leaf);
    nodes_[0].weights.push_back(1.0f / static_cast<float>(wanted));
  }
  leaf_count_ = wanted;
}

std::uint8_t EditorSplitTree::AddNode(EditorSplitOrientation orientation, std::uint8_t parent) {
  if (nodes_.size() >= kMaxNodes) {
    return kNoNode;
  }
  Node fresh;
  fresh.orientation = orientation;
  fresh.parent = parent;
  nodes_.push_back(fresh);
  return static_cast<std::uint8_t>(nodes_.size() - 1);
}

std::uint8_t EditorSplitTree::NodeForLeaf(std::size_t leaf) const {
  std::size_t seen = 0;
  std::uint8_t found = kNoNode;
  WalkDepthFirst(nodes(), root_, [&](std::uint8_t index) {
    if (!nodes_[index].leaf()) {
      return true;
    }
    if (seen++ == leaf) {
      found = index;
      return false;
    }
    return true;
  });
  return found;
}

std::size_t EditorSplitTree::LeafOrdinal(std::uint8_t node) const {
  if (node >= nodes_.size() || !nodes_[node].leaf()) {
    return kNoLeaf;
  }
  std::size_t seen = 0;
  std::size_t ordinal = kNoLeaf;
  WalkDepthFirst(nodes(), root_, [&](std::uint8_t index) {
    if (!nodes_[index].leaf()) {
      return true;
    }
    if (index == node) {
      ordinal = seen;
      return false;
    }
    ++seen;
    return true;
  });
  return ordinal;
}

std::size_t EditorSplitTree::CountLeaves() const {
  std::size_t leaves = 0;
  WalkDepthFirst(nodes(), root_, [&](std::uint8_t index) {
    leaves += nodes_[index].leaf() ? 1 : 0;
    return true;
  });
  return leaves;
}

void EditorSplitTree::Normalise(Node& branch) {
  float total = 0.0f;
  for (std::size_t i = 0; i < branch.weights.size(); ++i) {
    // A hand-edited session could carry a negative or non-finite weight straight
    // into layout; fold that back to an even share here rather than letting it
    // reach the geometry pass.
    if (!std::isfinite(branch.weights[i]) || branch.weights[i] <= 0.0f) {
      branch.weights[i] = 0.0f;
    }
    total += branch.weights[i];
  }
  if (branch.weights.empty()) {
    return;
  }
  if (total <= 0.0f) {
    const float even = 1.0f / static_cast<float>(branch.weights.size());
    for (std::size_t i = 0; i < branch.weights.size(); ++i) {
      branch.weights[i] = even;
    }
    return;
  }
  for (std::size_t i = 0; i < branch.weights.size(); ++i) {
    branch.weights[i] /= total;
  }
}

std::size_t EditorSplitTree::InsertLeaf(std::size_t leaf,
                                        EditorSplitOrientation orientation,
                                        bool before) {
  if (orientation == EditorSplitOrientation::None || full()) {
    return kNoLeaf;
  }
  const std::uint8_t target = NodeForLeaf(leaf);
  if (target == kNoNode) {
    return kNoLeaf;
  }

  const std::uint8_t parent = nodes_[target].parent;
  if (parent != kNoNode && nodes_[parent].orientation == orientation) {
    // The pane's own branch already splits this way: the new pane becomes a
    // sibling sharing the target's room, which is what keeps three side-by-side
    // panes a flat row instead of a nest of pairs.
    Node& branch = nodes_[parent];
    std::size_t position = 0;
    while (position < branch.children.size() && branch.children[position] != target) {
      ++position;
    }
    if (position == branch.children.size()) {
      return kNoLeaf;
    }
    const std::uint8_t fresh = AddNode(EditorSplitOrientation::None, parent);
    if (fresh == kNoNode) {
      return kNoLeaf;
    }
    Node& reloaded = nodes_[parent];
    const float half = reloaded.weights[position] * 0.5f;
    reloaded.weights[position] = half;
    const std::size_t at = before ? position : position + 1;
    reloaded.children.insert(at, fresh);
    reloaded.weights.insert(at, half);
    Normalise(reloaded);
    ++leaf_count_;
    return LeafOrdinal(fresh);
  }

  // Otherwise the leaf itself becomes the branch, keeping its slot in whatever
  // holds it, with the old pane and the new one as its two children.
  const std::uint8_t moved = AddNode(EditorSplitOrientation::None, target);
  const std::uint8_t fresh = AddNode(EditorSplitOrientation::None, target);
  if (moved == kNoNode || fresh == kNoNode) {
    Rebuild();
    return kNoLeaf;
  }
  Node& branch = nodes_[target];
  branch.orientation = orientation;
  branch.children.clear();
  branch.weights.clear();
  branch.children.push_back(before ? fresh : moved);
  branch.children.push_back(before ? moved : fresh);
  branch.weights.push_back(0.5f);
  branch.weights.push_back(0.5f);
  ++leaf_count_;
  return LeafOrdinal(fresh);
}

void EditorSplitTree::RemoveLeaf(std::size_t leaf) {
  if (leaf_count_ <= 1) {
    return;
  }
  const std::uint8_t target = NodeForLeaf(leaf);
  if (target == kNoNode) {
    return;
  }
  const std::uint8_t parent = nodes_[target].parent;
  if (parent == kNoNode) {
    Reset();
    return;
  }

  {
    Node& branch = nodes_[parent];
    std::size_t position = 0;
    while (position < branch.children.size() && branch.children[position] != target) {
      ++position;
    }
    if (position == branch.children.size()) {
      return;
    }
    branch.children.erase(position);
    branch.weights.erase(position);
    Normalise(branch);
  }

  // A branch left holding one child is not a split any more: splice that child
  // into the branch's own slot. If the child splits the same way as the
  // grandparent, its children flatten into that row instead, keeping the tree
  // canonical (no branch nested in a same-axis branch).
  if (nodes_[parent].children.size() == 1) {
    const std::uint8_t survivor = nodes_[parent].children[0];
    const std::uint8_t grandparent = nodes_[parent].parent;
    if (grandparent == kNoNode) {
      root_ = survivor;
      nodes_[survivor].parent = kNoNode;
    } else {
      Node& above = nodes_[grandparent];
      std::size_t slot = 0;
      while (slot < above.children.size() && above.children[slot] != parent) {
        ++slot;
      }
      if (slot < above.children.size()) {
        const float share = above.weights[slot];
        if (!nodes_[survivor].leaf() && nodes_[survivor].orientation == above.orientation) {
          const Node lifted = nodes_[survivor];
          above.children.erase(slot);
          above.weights.erase(slot);
          for (std::size_t i = 0; i < lifted.children.size(); ++i) {
            above.children.insert(slot + i, lifted.children[i]);
            above.weights.insert(slot + i, share * lifted.weights[i]);
            nodes_[lifted.children[i]].parent = grandparent;
          }
        } else {
          above.children[slot] = survivor;
          nodes_[survivor].parent = grandparent;
        }
        Normalise(above);
      }
    }
  }

  Rebuild();
}

void EditorSplitTree::Rebuild() {
  util::InlineVector<std::uint8_t, kMaxNodes> remap;
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    remap.push_back(kNoNode);
  }
  util::InlineVector<Node, kMaxNodes> compacted;
  WalkDepthFirst(nodes(), root_, [&](std::uint8_t index) {
    remap[index] = static_cast<std::uint8_t>(compacted.size());
    compacted.push_back(nodes_[index]);
    return true;
  });
  for (std::size_t i = 0; i < compacted.size(); ++i) {
    Node& current = compacted[i];
    if (current.parent != kNoNode) {
      current.parent = remap[current.parent];
    }
    for (std::size_t c = 0; c < current.children.size(); ++c) {
      current.children[c] = remap[current.children[c]];
    }
  }
  nodes_ = compacted;
  root_ = nodes_.empty() ? 0 : 0;
  if (nodes_.empty()) {
    Reset();
    return;
  }
  leaf_count_ = CountLeaves();
}

bool EditorSplitTree::ResizeDivider(std::uint8_t node, std::size_t boundary, float first_share) {
  if (node >= nodes_.size() || nodes_[node].leaf() ||
      boundary + 1 >= nodes_[node].children.size() || !std::isfinite(first_share)) {
    return false;
  }
  Node& branch = nodes_[node];
  const float pair = branch.weights[boundary] + branch.weights[boundary + 1];
  const float share = std::clamp(first_share, kMinDividerShare, 1.0f - kMinDividerShare);
  branch.weights[boundary] = pair * share;
  branch.weights[boundary + 1] = pair * (1.0f - share);
  return true;
}

bool EditorSplitTree::ResetDivider(std::uint8_t node, std::size_t boundary) {
  return ResizeDivider(node, boundary, 0.5f);
}

EditorSplitTreeRecord EditorSplitTree::Flatten() const {
  EditorSplitTreeRecord record;
  WalkDepthFirst(nodes(), root_, [&](std::uint8_t index) {
    const Node& current = nodes_[index];
    EditorSplitNodeRecord entry;
    entry.orientation = current.orientation;
    entry.weights = current.weights;
    record.push_back(entry);
    return true;
  });
  return record;
}

namespace {

// Consume one node (and, recursively, its children) from a pre-order record.
// Returns false on anything malformed; `cursor` ends past the subtree.
bool LoadSubtree(const EditorSplitTreeRecord& record,
                 std::size_t& cursor,
                 std::uint8_t parent,
                 util::InlineVector<EditorSplitNode, EditorSplitTree::kMaxNodes>& out,
                 std::size_t& leaves) {
  if (cursor >= record.size() || out.size() >= EditorSplitTree::kMaxNodes) {
    return false;
  }
  const EditorSplitNodeRecord& entry = record[cursor++];
  const std::uint8_t self = static_cast<std::uint8_t>(out.size());
  EditorSplitNode node;
  node.orientation = entry.orientation;
  node.parent = parent;
  out.push_back(node);
  if (entry.orientation == EditorSplitOrientation::None) {
    ++leaves;
    return entry.weights.empty() && leaves <= kMaxEditorGroups;
  }
  if (entry.orientation != EditorSplitOrientation::Vertical &&
      entry.orientation != EditorSplitOrientation::Horizontal) {
    return false;
  }
  if (entry.weights.size() < 2) {
    return false;  // a branch is a split; one child is not one.
  }
  for (std::size_t i = 0; i < entry.weights.size(); ++i) {
    const std::uint8_t child = static_cast<std::uint8_t>(out.size());
    if (!LoadSubtree(record, cursor, self, out, leaves)) {
      return false;
    }
    out[self].children.push_back(child);
    out[self].weights.push_back(entry.weights[i]);
  }
  return true;
}

}  // namespace

bool EditorSplitTree::Load(const EditorSplitTreeRecord& record) {
  util::InlineVector<Node, kMaxNodes> loaded;
  std::size_t cursor = 0;
  std::size_t leaves = 0;
  if (!LoadSubtree(record, cursor, kNoNode, loaded, leaves) || cursor != record.size() ||
      leaves == 0 || leaves > kMaxEditorGroups) {
    Reset();
    return false;
  }
  nodes_ = loaded;
  root_ = 0;
  leaf_count_ = leaves;
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (!nodes_[i].leaf()) {
      Normalise(nodes_[i]);
    }
  }
  return true;
}

bool operator==(const EditorSplitTree& lhs, const EditorSplitTree& rhs) {
  // STRUCTURAL, not index-by-index. `AddNode` appends, so a tree that has been
  // edited (insert after a remove, say) carries the same shape under a different
  // node numbering than the identical tree built by `Load`. Comparing the storage
  // called those two unequal -- a trap for anything using this for "did the layout
  // change", and it made a correct structure fail its own round-trip test. The
  // flat pre-order form IS the canonical shape, so comparing it is the definition.
  return lhs.leaf_count_ == rhs.leaf_count_ && lhs.Flatten() == rhs.Flatten();
}

}  // namespace microide::workspace
