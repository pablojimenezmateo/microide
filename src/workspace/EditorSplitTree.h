#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "util/InlineVector.h"

namespace microide::workspace {

// Which way a split divides the pane it is applied to. `Vertical` puts the two
// panes side by side (a vertical divider between them); `Horizontal` stacks
// them. `None` is only ever the orientation of a LEAF node in the split tree.
enum class EditorSplitOrientation : std::uint8_t {
  None = 0,
  Vertical = 1,
  Horizontal = 2,
};

// The editor area hosts at most this many groups. This is the single definition
// of that cap: the split tree, the per-group tab-strip caches, the surface split
// and the session decoder all read it, and it is the capacity of every per-group
// inline container, so raising it cannot leave one of them behind.
//
// Eight is a product limit, not a structural one — the tree below is n-ary and
// would carry more — chosen so every layout container stays heap-free (the pane
// rects are rebuilt three times per mouse-motion event) and so the cap is far
// past any workflow a single window can usefully show.
inline constexpr std::size_t kMaxEditorGroups = 8;

// Sentinel node index, and the node budget an n-ary tree over `kMaxEditorGroups`
// leaves can reach (leaves + at most leaves-1 branches).
inline constexpr std::uint8_t kNoEditorSplitNode = 0xFF;
inline constexpr std::size_t kMaxEditorSplitNodes = kMaxEditorGroups * 2 - 1;

// One node of the tree below. Deliberately at NAMESPACE scope rather than nested
// in `EditorSplitTree`: a class nested in a still-incomplete enclosing class does
// not have its default-member-initializers parsed yet, so clang evaluates
// `std::is_constructible_v<Node>` as FALSE while instantiating the enclosing
// `InlineVector<Node, N>` member and caches that answer (the same trap
// `PluginGhostText` documents in WorkspaceProjectState.h).
struct EditorSplitNode {
  // `None` marks a leaf; a branch carries its axis here.
  EditorSplitOrientation orientation = EditorSplitOrientation::None;
  std::uint8_t parent = kNoEditorSplitNode;
  util::InlineVector<std::uint8_t, kMaxEditorGroups> children;
  // Parallel to `children`; normalised to sum to 1 after every structural edit.
  util::InlineVector<float, kMaxEditorGroups> weights;

  bool leaf() const { return orientation == EditorSplitOrientation::None; }
};

// One node in the flat pre-order form the tree is persisted in: a leaf carries no
// weights; a branch carries one weight per child, and its children are the next
// entries in the stream (recursively). Small enough to hand around by value and
// heap-free like everything else here.
struct EditorSplitNodeRecord {
  EditorSplitOrientation orientation = EditorSplitOrientation::None;
  util::InlineVector<float, kMaxEditorGroups> weights;

  friend bool operator==(const EditorSplitNodeRecord&, const EditorSplitNodeRecord&) = default;
};
using EditorSplitTreeRecord = util::InlineVector<EditorSplitNodeRecord, kMaxEditorSplitNodes>;

// The layout tree over the editor groups: VS Code's editor grid.
//
// A LEAF is one editor group (its own tab strip and surface). A BRANCH lays its
// children out along one axis with per-child weights. Leaves in in-order
// traversal are exactly `ProjectWorkspaceState::editor_groups` in order, so a
// group index IS a leaf ordinal and every existing group-indexed cache, focus
// index and tab-strip slot keeps its meaning — the tree only says where each
// group sits and how much room it gets.
//
// Canonical form: a branch never has fewer than two children, and no child of a
// branch is a branch with the SAME orientation (those are flattened into the
// parent, the way VS Code's grid does). That is what makes "split this pane the
// way its parent already splits" an insert next to it rather than a nested pair,
// and it keeps the tree's depth proportional to the number of orientation
// changes rather than to the number of splits.
//
// Heap-free: `kMaxEditorGroups` leaves bound the node count at 2N-1, so the
// whole structure is one inline array. It is copied with the project state and
// walked on every hit-test, so it owns no allocation at all.
class EditorSplitTree {
 public:
  using Node = EditorSplitNode;
  static constexpr std::uint8_t kNoNode = kNoEditorSplitNode;
  static constexpr std::size_t kNoLeaf = static_cast<std::size_t>(-1);
  static constexpr std::size_t kMaxNodes = kMaxEditorSplitNodes;

  // A fresh tree is a single leaf: one editor group, no divider.
  EditorSplitTree() { Reset(); }

  void Reset();
  // Replace the tree with `leaves` panes side by side (or stacked) in equal
  // shares. Used by session restore when the persisted structure is missing or
  // does not match the restored group count, so the two can never disagree.
  void ResetToEvenSplit(std::size_t leaves, EditorSplitOrientation orientation);

  std::size_t leaf_count() const { return leaf_count_; }
  bool is_split() const { return leaf_count_ > 1; }
  bool full() const { return leaf_count_ >= kMaxEditorGroups; }

  std::uint8_t root() const { return root_; }
  std::size_t node_count() const { return nodes_.size(); }
  const Node& node(std::uint8_t index) const { return nodes_[index]; }
  std::span<const Node> nodes() const { return {nodes_.data(), nodes_.size()}; }

  // Node holding leaf ordinal `leaf`, or `kNoNode`.
  std::uint8_t NodeForLeaf(std::size_t leaf) const;
  // Ordinal of a leaf node, or `kNoLeaf` when `node` is not a leaf.
  std::size_t LeafOrdinal(std::uint8_t node) const;

  // Split the pane holding `leaf` along `orientation`, putting the new pane on
  // the leading (`before`) or trailing side of it. Returns the new pane's leaf
  // ordinal — which is the index the caller must insert its editor group at — or
  // `kNoLeaf` when the tree is full or `leaf` does not exist.
  std::size_t InsertLeaf(std::size_t leaf, EditorSplitOrientation orientation, bool before);

  // Drop a pane, giving its room back to its siblings and collapsing any branch
  // left with a single child. A tree of one leaf is left alone: the editor area
  // always has at least one group.
  void RemoveLeaf(std::size_t leaf);

  // Relocate an existing pane next to `target`, on its leading (`before`) or
  // trailing side along `orientation`. Returns the moved pane's ordinal in the
  // NEW numbering, or `kNoLeaf` (tree untouched) when the addresses do not name
  // two distinct live leaves.
  //
  // Remove-then-insert, deliberately: the pane count never rises, so this is the
  // one structural edit a FULL grid can still take -- both the directional
  // "move pane left/right/up/down" verbs and the drop that hands another pane's
  // last tab to an edge go through it (TD-2026-08-18-265, TD-2026-08-18-266).
  // The caller must reorder its parallel `editor_groups` the same way: erase at
  // `from`, re-insert at the returned ordinal.
  std::size_t MoveLeaf(std::size_t from,
                       std::size_t target,
                       EditorSplitOrientation orientation,
                       bool before);

  // Move the divider between `boundary` and `boundary + 1` of branch `node`, as
  // a share of the two panes' COMBINED extent (VS Code moves only the pair the
  // divider touches; everything else keeps its size). Returns false when the
  // address does not name a live divider.
  bool ResizeDivider(std::uint8_t node, std::size_t boundary, float first_share);
  // Restore an even share between that pair (divider double-click).
  bool ResetDivider(std::uint8_t node, std::size_t boundary);

  // Smallest share of a pair either side may be squeezed to. Keeps a pane from
  // being dragged to nothing, and matches the old single-split 0.1/0.9 clamp.
  static constexpr float kMinDividerShare = 0.1f;

  // Flat pre-order form for persistence, and its inverse. `Load` rejects a stream
  // that is not a well-formed tree within the caps (a branch with fewer than two
  // children, a truncated stream, trailing junk) and leaves a single leaf behind,
  // so a corrupt session cannot produce a layout with no pane for a group.
  EditorSplitTreeRecord Flatten() const;
  bool Load(const EditorSplitTreeRecord& record);

  friend bool operator==(const EditorSplitTree& lhs, const EditorSplitTree& rhs);

  // Identity of this tree's SHAPE, for memo keys. Every structural edit and every
  // divider move stamps a fresh process-unique value, and a copy carries the value
  // of what it copied, so: equal revisions imply identical shape (the converse does
  // not hold -- two trees built the same way independently differ here, and a memo
  // keyed on it simply recomputes once). This is what lets the per-frame pane-rect
  // walk be served from a cache without a comparison walk of its own
  // (TD-2026-08-30-280). Not part of `operator==`, which stays structural.
  std::uint64_t revision() const { return revision_; }

 private:
  void Touch();

  std::uint8_t AddNode(EditorSplitOrientation orientation, std::uint8_t parent);
  // Drop every node no longer reachable from the root and renumber what is left,
  // so index fixups never have to be threaded through the structural edits.
  void Rebuild();
  void Normalise(Node& branch);
  std::size_t CountLeaves() const;

  util::InlineVector<Node, kMaxNodes> nodes_;
  std::uint8_t root_ = 0;
  std::size_t leaf_count_ = 1;
  std::uint64_t revision_ = 0;
};

}  // namespace microide::workspace
