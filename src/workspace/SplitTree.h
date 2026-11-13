#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "util/InlineVector.h"

namespace microide::workspace {

// Which way a split divides the pane it is applied to. `Vertical` puts the two
// panes side by side (a vertical divider between them); `Horizontal` stacks
// them. `None` is only ever the orientation of a LEAF node in the split tree.
enum class SplitOrientation : std::uint8_t {
  None = 0,
  Vertical = 1,
  Horizontal = 2,
};

// A split grid holds at most this many panes. This is the single definition of
// that cap for EVERY surface laid out by a `SplitTree` -- the editor area's
// groups (`kMaxEditorGroups`) and a terminal tab's panes (`kMaxTerminalPanes`)
// -- and the capacity of every per-pane inline container (the region layout,
// the per-group tab-strip caches, the session decoder), so raising it cannot
// leave one of them behind.
//
// Eight is a product limit, not a structural one -- the tree below is n-ary and
// would carry more -- chosen so every layout container stays heap-free (the pane
// rects are rebuilt several times per mouse-motion event) and so the cap is far
// past any workflow a single window can usefully show.
inline constexpr std::size_t kMaxSplitLeaves = 8;

// The editor area's groups and a terminal tab's panes: one cap, two names for
// the call sites that mean one surface.
inline constexpr std::size_t kMaxEditorGroups = kMaxSplitLeaves;
inline constexpr std::size_t kMaxTerminalPanes = kMaxSplitLeaves;

// Sentinel node index, and the node budget an n-ary tree over `kMaxSplitLeaves`
// leaves can reach (leaves + at most leaves-1 branches).
inline constexpr std::uint8_t kNoSplitNode = 0xFF;
inline constexpr std::size_t kMaxSplitNodes = kMaxSplitLeaves * 2 - 1;

// One node of the tree below. Deliberately at NAMESPACE scope rather than nested
// in `SplitTree`: a class nested in a still-incomplete enclosing class does
// not have its default-member-initializers parsed yet, so clang evaluates
// `std::is_constructible_v<Node>` as FALSE while instantiating the enclosing
// `InlineVector<Node, N>` member and caches that answer (the same trap
// `PluginGhostText` documents in WorkspaceProjectState.h).
struct SplitNode {
  // `None` marks a leaf; a branch carries its axis here.
  SplitOrientation orientation = SplitOrientation::None;
  std::uint8_t parent = kNoSplitNode;
  util::InlineVector<std::uint8_t, kMaxSplitLeaves> children;
  // Parallel to `children`; normalised to sum to 1 after every structural edit.
  util::InlineVector<float, kMaxSplitLeaves> weights;

  bool leaf() const { return orientation == SplitOrientation::None; }
};

// One node in the flat pre-order form the tree is persisted in: a leaf carries no
// weights; a branch carries one weight per child, and its children are the next
// entries in the stream (recursively). Small enough to hand around by value and
// heap-free like everything else here.
struct SplitNodeRecord {
  SplitOrientation orientation = SplitOrientation::None;
  util::InlineVector<float, kMaxSplitLeaves> weights;

  friend bool operator==(const SplitNodeRecord&, const SplitNodeRecord&) = default;
};
using SplitTreeRecord = util::InlineVector<SplitNodeRecord, kMaxSplitNodes>;

// The layout tree over a surface's panes: VS Code's editor grid, and the
// arrangement of a terminal tab's panes.
//
// A LEAF is one pane (an editor group with its own tab strip and surface, or one
// terminal session). A BRANCH lays its children out along one axis with
// per-child weights. Leaves in in-order traversal are exactly the owner's pane
// vector in order (`ProjectWorkspaceState::editor_groups`,
// `TerminalTabState::panes`), so a pane index IS a leaf ordinal and every
// index-keyed cache and focus index keeps its meaning -- the tree only says where
// each pane sits and how much room it gets. `ComputeSplitRegions`
// (SplitTreeLayout.h) is the one walk that turns it into rects.
//
// Canonical form: a branch never has fewer than two children, and no child of a
// branch is a branch with the SAME orientation (those are flattened into the
// parent, the way VS Code's grid does). That is what makes "split this pane the
// way its parent already splits" an insert next to it rather than a nested pair,
// and it keeps the tree's depth proportional to the number of orientation
// changes rather than to the number of splits.
//
// Heap-free: `kMaxSplitLeaves` leaves bound the node count at 2N-1, so the
// whole structure is one inline array. It is copied with the project state and
// walked on every hit-test, so it owns no allocation at all.
class SplitTree {
 public:
  using Node = SplitNode;
  static constexpr std::uint8_t kNoNode = kNoSplitNode;
  static constexpr std::size_t kNoLeaf = static_cast<std::size_t>(-1);
  static constexpr std::size_t kMaxNodes = kMaxSplitNodes;

  // A fresh tree is a single leaf: one editor group, no divider.
  SplitTree() { Reset(); }

  void Reset();
  // Replace the tree with `leaves` panes side by side (or stacked) in equal
  // shares. Used by session restore when the persisted structure is missing or
  // does not match the restored group count, so the two can never disagree.
  void ResetToEvenSplit(std::size_t leaves, SplitOrientation orientation);

  std::size_t leaf_count() const { return leaf_count_; }
  bool is_split() const { return leaf_count_ > 1; }
  bool full() const { return leaf_count_ >= kMaxSplitLeaves; }

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
  std::size_t InsertLeaf(std::size_t leaf, SplitOrientation orientation, bool before);

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
                       SplitOrientation orientation,
                       bool before);

  // Seat a whole other tree beside `target`, on its leading (`before`) or trailing
  // side along `orientation`: the drop of a split terminal tab onto another tab's
  // pane edge. The graft keeps its own shape and shares; when its root splits the
  // same way as the branch it lands in, its children join that row (canonical
  // form). Returns the ordinal of the graft's FIRST leaf in the new numbering --
  // its leaves are contiguous from there, in their own order, which is where the
  // caller must splice its pane vector -- or `kNoLeaf` (tree untouched) when the
  // combined leaf count would pass the cap or `target` is not a live leaf.
  std::size_t GraftTree(std::size_t target,
                        SplitOrientation orientation,
                        bool before,
                        const SplitTree& graft);

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
  SplitTreeRecord Flatten() const;
  bool Load(const SplitTreeRecord& record);

  friend bool operator==(const SplitTree& lhs, const SplitTree& rhs);

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

  std::uint8_t AddNode(SplitOrientation orientation, std::uint8_t parent);
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
