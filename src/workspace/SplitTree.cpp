#include "workspace/SplitTree.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace microide::workspace {

namespace {

// Depth-first, children left to right: the order panes appear on screen, and so
// the order leaf ordinals (== pane indices) are assigned in.
template <typename Visit>
void WalkDepthFirst(std::span<const SplitTree::Node> nodes,
                    std::uint8_t root,
                    Visit&& visit) {
  if (root >= nodes.size()) {
    return;
  }
  util::InlineVector<std::uint8_t, SplitTree::kMaxNodes> stack;
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
    const SplitTree::Node& current = nodes[index];
    for (std::size_t i = current.children.size(); i > 0; --i) {
      stack.push_back(current.children[i - 1]);
    }
  }
}

}  // namespace

void SplitTree::Touch() {
  // Process-wide so that two DIFFERENT trees can never share a stamp: a
  // per-instance counter would let project A's tree at edit #5 collide with
  // project B's at edit #5 the moment the shell switched between them.
  static std::atomic<std::uint64_t> next_revision{0};
  revision_ = next_revision.fetch_add(1, std::memory_order_relaxed) + 1;
}

void SplitTree::Reset() {
  nodes_.clear();
  nodes_.push_back(Node{});
  root_ = 0;
  leaf_count_ = 1;
  Touch();
}

void SplitTree::ResetToEvenSplit(std::size_t leaves, SplitOrientation orientation) {
  Reset();
  const std::size_t wanted = std::clamp<std::size_t>(leaves, 1, kMaxSplitLeaves);
  if (wanted < 2 || orientation == SplitOrientation::None) {
    return;
  }
  nodes_[0].orientation = orientation;
  for (std::size_t i = 0; i < wanted; ++i) {
    const std::uint8_t leaf = AddNode(SplitOrientation::None, 0);
    nodes_[0].children.push_back(leaf);
    nodes_[0].weights.push_back(1.0f / static_cast<float>(wanted));
  }
  leaf_count_ = wanted;
  Touch();
}

std::uint8_t SplitTree::AddNode(SplitOrientation orientation, std::uint8_t parent) {
  if (nodes_.size() >= kMaxNodes) {
    return kNoNode;
  }
  Node fresh;
  fresh.orientation = orientation;
  fresh.parent = parent;
  nodes_.push_back(fresh);
  return static_cast<std::uint8_t>(nodes_.size() - 1);
}

std::uint8_t SplitTree::NodeForLeaf(std::size_t leaf) const {
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

std::size_t SplitTree::LeafOrdinal(std::uint8_t node) const {
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

std::size_t SplitTree::CountLeaves() const {
  std::size_t leaves = 0;
  WalkDepthFirst(nodes(), root_, [&](std::uint8_t index) {
    leaves += nodes_[index].leaf() ? 1 : 0;
    return true;
  });
  return leaves;
}

void SplitTree::Normalise(Node& branch) {
  if (branch.weights.empty()) {
    return;
  }
  float total = 0.0f;
  bool invalid = false;
  for (std::size_t i = 0; i < branch.weights.size(); ++i) {
    // A hand-edited session could carry a negative or non-finite weight straight
    // into layout; fold the whole branch back to even shares rather than letting
    // a zero-width pane reach the geometry pass.
    if (!std::isfinite(branch.weights[i]) || branch.weights[i] <= 0.0f) {
      invalid = true;
      break;
    }
    total += branch.weights[i];
  }
  if (invalid || total <= 0.0f) {
    const float even = 1.0f / static_cast<float>(branch.weights.size());
    for (std::size_t i = 0; i < branch.weights.size(); ++i) {
      branch.weights[i] = even;
    }
    return;
  }
  // Idempotent within float precision: a branch whose shares already sum to one
  // (up to the rounding a divider drag leaves behind) is left byte-for-byte as
  // it is. Dividing through by 0.99999994 moved every weight by an ulp, so a
  // tree did not equal its own persisted-and-reloaded form after one drag.
  if (std::abs(total - 1.0f) <= 1e-5f) {
    return;
  }
  for (std::size_t i = 0; i < branch.weights.size(); ++i) {
    branch.weights[i] /= total;
  }
}

std::size_t SplitTree::InsertLeaf(std::size_t leaf,
                                        SplitOrientation orientation,
                                        bool before) {
  if (orientation == SplitOrientation::None || full()) {
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
    const std::uint8_t fresh = AddNode(SplitOrientation::None, parent);
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
    Touch();
    return LeafOrdinal(fresh);
  }

  // Otherwise the leaf itself becomes the branch, keeping its slot in whatever
  // holds it, with the old pane and the new one as its two children.
  const std::uint8_t moved = AddNode(SplitOrientation::None, target);
  const std::uint8_t fresh = AddNode(SplitOrientation::None, target);
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
  Touch();
  return LeafOrdinal(fresh);
}

std::size_t SplitTree::MoveLeaf(std::size_t from,
                                      std::size_t target,
                                      SplitOrientation orientation,
                                      bool before) {
  if (orientation == SplitOrientation::None || from == target || from >= leaf_count_ ||
      target >= leaf_count_ || leaf_count_ < 2) {
    return kNoLeaf;
  }
  // Remove first: the grid may already be FULL, and inserting before removing
  // would momentarily need a leaf the node budget cannot hold. Nothing below can
  // fail once the removal has happened -- the tree is provably not full, and the
  // adjusted target is a live ordinal -- so this never leaves a half-applied move.
  RemoveLeaf(from);
  return InsertLeaf(target > from ? target - 1 : target, orientation, before);
}

std::size_t SplitTree::GraftTree(std::size_t target,
                                 SplitOrientation orientation,
                                 bool before,
                                 const SplitTree& graft) {
  if (orientation == SplitOrientation::None || target >= leaf_count_ ||
      leaf_count_ + graft.leaf_count() > kMaxSplitLeaves) {
    return kNoLeaf;
  }
  // Seat one leaf where the graft goes, with every canonical-form rule InsertLeaf
  // already applies; a single-leaf graft is exactly that and nothing more.
  const std::size_t seat = InsertLeaf(target, orientation, before);
  if (seat == kNoLeaf || !graft.is_split()) {
    return seat;
  }
  const std::uint8_t seat_node = NodeForLeaf(seat);
  if (seat_node == kNoNode) {
    return kNoLeaf;
  }

  // Copy the graft's nodes in, its root INTO the seat (which keeps the seat's
  // place in its parent) and every other node appended. The budget holds: the
  // tree is compact after InsertLeaf (it never leaves garbage), so it has at most
  // 2(L+1)-1 nodes, the graft brings G-node count minus its reused root, and
  // 2(L+1)-1 + 2G-2 = 2(L+G)-1, inside the node budget for L+G <= the leaf cap.
  util::InlineVector<std::uint8_t, kMaxNodes> remap;
  for (std::size_t i = 0; i < graft.node_count(); ++i) {
    remap.push_back(kNoNode);
  }
  remap[graft.root()] = seat_node;
  for (std::size_t i = 0; i < graft.node_count(); ++i) {
    if (i == graft.root()) {
      continue;
    }
    remap[i] = AddNode(SplitOrientation::None, kNoNode);
    if (remap[i] == kNoNode) {
      // Unreachable by the arithmetic above; drop what was added and undo the seat
      // rather than leave a half-grafted tree.
      RemoveLeaf(LeafOrdinal(seat_node));
      return kNoLeaf;
    }
  }
  const std::uint8_t seat_parent = nodes_[seat_node].parent;
  for (std::size_t i = 0; i < graft.node_count(); ++i) {
    Node copy = graft.node(static_cast<std::uint8_t>(i));
    copy.parent = i == graft.root() ? seat_parent : remap[copy.parent];
    for (std::size_t c = 0; c < copy.children.size(); ++c) {
      copy.children[c] = remap[copy.children[c]];
    }
    nodes_[remap[i]] = copy;
  }

  // Canonical form: a graft whose root splits the way its new parent does is a
  // run of siblings in that row, not a nested branch.
  if (seat_parent != kNoNode && nodes_[seat_parent].orientation == nodes_[seat_node].orientation) {
    Node& above = nodes_[seat_parent];
    std::size_t slot = 0;
    while (slot < above.children.size() && above.children[slot] != seat_node) {
      ++slot;
    }
    if (slot < above.children.size()) {
      const Node lifted = nodes_[seat_node];
      const float share = above.weights[slot];
      above.children.erase(slot);
      above.weights.erase(slot);
      for (std::size_t i = 0; i < lifted.children.size(); ++i) {
        above.children.insert(slot + i, lifted.children[i]);
        above.weights.insert(slot + i, share * lifted.weights[i]);
        nodes_[lifted.children[i]].parent = seat_parent;
      }
      Normalise(above);
    }
  }
  // Drops the seat node when it was flattened away and recounts the leaves. The
  // graft occupies the seat's place in leaf order, so its first leaf keeps the
  // seat's ordinal.
  Rebuild();
  return seat;
}

void SplitTree::RemoveLeaf(std::size_t leaf) {
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

void SplitTree::Rebuild() {
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
  // The walk starts at the old root, so the old root is the first node pushed and
  // the compacted root is always index 0. (This was written as a ternary whose two
  // arms were both 0 — it read as if empty and non-empty differed here, and they do
  // not; the empty case is handled by the Reset below.)
  root_ = 0;
  if (nodes_.empty()) {
    Reset();
    return;
  }
  leaf_count_ = CountLeaves();
  Touch();
}

bool SplitTree::ResizeDivider(std::uint8_t node, std::size_t boundary, float first_share) {
  if (node >= nodes_.size() || nodes_[node].leaf() ||
      boundary + 1 >= nodes_[node].children.size() || !std::isfinite(first_share)) {
    return false;
  }
  Node& branch = nodes_[node];
  const float pair = branch.weights[boundary] + branch.weights[boundary + 1];
  const float share = std::clamp(first_share, kMinDividerShare, 1.0f - kMinDividerShare);
  branch.weights[boundary] = pair * share;
  // The remainder, not `pair * (1 - share)`: the two products need not sum back
  // to `pair` in floats, and the branch total would drift an ulp per drag.
  branch.weights[boundary + 1] = pair - branch.weights[boundary];
  Touch();
  return true;
}

bool SplitTree::ResetDivider(std::uint8_t node, std::size_t boundary) {
  return ResizeDivider(node, boundary, 0.5f);
}

SplitTreeRecord SplitTree::Flatten() const {
  SplitTreeRecord record;
  WalkDepthFirst(nodes(), root_, [&](std::uint8_t index) {
    const Node& current = nodes_[index];
    SplitNodeRecord entry;
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
bool LoadSubtree(const SplitTreeRecord& record,
                 std::size_t& cursor,
                 std::uint8_t parent,
                 util::InlineVector<SplitNode, SplitTree::kMaxNodes>& out,
                 std::size_t& leaves) {
  if (cursor >= record.size() || out.size() >= SplitTree::kMaxNodes) {
    return false;
  }
  const SplitNodeRecord& entry = record[cursor++];
  const std::uint8_t self = static_cast<std::uint8_t>(out.size());
  SplitNode node;
  node.orientation = entry.orientation;
  node.parent = parent;
  out.push_back(node);
  if (entry.orientation == SplitOrientation::None) {
    ++leaves;
    return entry.weights.empty() && leaves <= kMaxSplitLeaves;
  }
  if (entry.orientation != SplitOrientation::Vertical &&
      entry.orientation != SplitOrientation::Horizontal) {
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

bool SplitTree::Load(const SplitTreeRecord& record) {
  util::InlineVector<Node, kMaxNodes> loaded;
  std::size_t cursor = 0;
  std::size_t leaves = 0;
  if (!LoadSubtree(record, cursor, kNoNode, loaded, leaves) || cursor != record.size() ||
      leaves == 0 || leaves > kMaxSplitLeaves) {
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
  Touch();
  return true;
}

bool operator==(const SplitTree& lhs, const SplitTree& rhs) {
  // STRUCTURAL, not index-by-index. `AddNode` appends, so a tree that has been
  // edited (insert after a remove, say) carries the same shape under a different
  // node numbering than the identical tree built by `Load`. Comparing the storage
  // called those two unequal -- a trap for anything using this for "did the layout
  // change", and it made a correct structure fail its own round-trip test. The
  // flat pre-order form IS the canonical shape, so comparing it is the definition.
  return lhs.leaf_count_ == rhs.leaf_count_ && lhs.Flatten() == rhs.Flatten();
}

}  // namespace microide::workspace
