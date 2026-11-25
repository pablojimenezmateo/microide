#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "util/InlineVector.h"

namespace microide::workspace {

enum class EditorSplitOrientation : std::uint8_t {
  None = 0,
  Vertical = 1,
  Horizontal = 2,
};

inline constexpr std::size_t kMaxEditorGroups = 8;

inline constexpr std::uint8_t kNoEditorSplitNode = 0xFF;
inline constexpr std::size_t kMaxEditorSplitNodes = kMaxEditorGroups * 2 - 1;

struct EditorSplitNode {
  EditorSplitOrientation orientation = EditorSplitOrientation::None;
  std::uint8_t parent = kNoEditorSplitNode;
  util::InlineVector<std::uint8_t, kMaxEditorGroups> children;
  util::InlineVector<float, kMaxEditorGroups> weights;

  bool leaf() const { return orientation == EditorSplitOrientation::None; }
};

struct EditorSplitNodeRecord {
  EditorSplitOrientation orientation = EditorSplitOrientation::None;
  util::InlineVector<float, kMaxEditorGroups> weights;

  friend bool operator==(const EditorSplitNodeRecord&, const EditorSplitNodeRecord&) = default;
};
using EditorSplitTreeRecord = util::InlineVector<EditorSplitNodeRecord, kMaxEditorSplitNodes>;

class EditorSplitTree {
 public:
  using Node = EditorSplitNode;
  static constexpr std::uint8_t kNoNode = kNoEditorSplitNode;
  static constexpr std::size_t kNoLeaf = static_cast<std::size_t>(-1);
  static constexpr std::size_t kMaxNodes = kMaxEditorSplitNodes;

  EditorSplitTree() { Reset(); }

  void Reset();
  void ResetToEvenSplit(std::size_t leaves, EditorSplitOrientation orientation);

  std::size_t leaf_count() const { return leaf_count_; }
  bool is_split() const { return leaf_count_ > 1; }
  bool full() const { return leaf_count_ >= kMaxEditorGroups; }

  std::uint8_t root() const { return root_; }
  std::size_t node_count() const { return nodes_.size(); }
  const Node& node(std::uint8_t index) const { return nodes_[index]; }
  std::span<const Node> nodes() const { return {nodes_.data(), nodes_.size()}; }

  std::uint8_t NodeForLeaf(std::size_t leaf) const;
  std::size_t LeafOrdinal(std::uint8_t node) const;

  std::size_t InsertLeaf(std::size_t leaf, EditorSplitOrientation orientation, bool before);

  void RemoveLeaf(std::size_t leaf);

  std::size_t MoveLeaf(std::size_t from,
                       std::size_t target,
                       EditorSplitOrientation orientation,
                       bool before);

  bool ResizeDivider(std::uint8_t node, std::size_t boundary, float first_share);
  bool ResetDivider(std::uint8_t node, std::size_t boundary);

  static constexpr float kMinDividerShare = 0.1f;

  EditorSplitTreeRecord Flatten() const;
  bool Load(const EditorSplitTreeRecord& record);

  friend bool operator==(const EditorSplitTree& lhs, const EditorSplitTree& rhs);

  std::uint64_t revision() const { return revision_; }

 private:
  void Touch();

  std::uint8_t AddNode(EditorSplitOrientation orientation, std::uint8_t parent);
  void Rebuild();
  void Normalise(Node& branch);
  std::size_t CountLeaves() const;

  util::InlineVector<Node, kMaxNodes> nodes_;
  std::uint8_t root_ = 0;
  std::size_t leaf_count_ = 1;
  std::uint64_t revision_ = 0;
};

}
