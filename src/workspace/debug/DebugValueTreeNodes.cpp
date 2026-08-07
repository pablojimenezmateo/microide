#include "workspace/debug/DebugValueTree.h"

#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

// Node storage and identity for DebugValueTree: the id→slot mapping, insertion,
// subtree teardown, reference rebinding, and the path key that survives a stop.
// Split from DebugValueTree.cpp (which keeps the fetch/flatten/edit behaviour) to
// stay under the debug-subsystem TU cap rather than raising it.

void DebugValueTree::Clear() {
  DropAllNodes();
  roots_.clear();
  rows_.clear();
  truncated_ = false;
  // next_id_ is intentionally NOT reset: node ids stay globally monotonic for the
  // life of the tree so a stale async response (a setVariable/variables reply
  // issued against a previous stop/frame) can never alias a freshly-created node
  // that happens to reuse the same id. This is defense in depth behind the
  // per-stop / per-eval generation guards in DebugService.
  selected_row_ = 0;
  editing_node_.reset();
  edit_buffer_.SetText({});
  // NB: expanded_paths_ and the one-shot default-expansion flag intentionally survive
  // Clear() — it runs on every stop/frame focus, so wiping them here would re-open the
  // default scope on every stop and lose a user's collapse. Session-scoped reset lives
  // in ResetExpansionForNewSession().
}

void DebugValueTree::ClearRoots() {
  DropAllNodes();
  roots_.clear();
  truncated_ = false;
  // next_id_ stays monotonic across rebuilds — see Clear().
  editing_node_.reset();
}

void DebugValueTree::DropAllNodes() {
  nodes_.clear();
  reference_to_node_.clear();
  live_nodes_ = 0;
  // Re-base the slot array on the next id that will be handed out. This is the
  // single point that keeps `nodes_[id - id_base_]` consistent with ids that are
  // never reused, and it is why an id from a previous stop resolves to nullptr
  // rather than aliasing a fresh node. `nodes_.clear()` keeps the capacity, so a
  // per-stop rebuild of the same size does not re-grow the array.
  id_base_ = next_id_;
}

DebugValueTree::Node* DebugValueTree::FindNode(std::uint32_t id) {
  return const_cast<Node*>(std::as_const(*this).FindNode(id));
}

const DebugValueTree::Node* DebugValueTree::FindNode(std::uint32_t id) const {
  if (id < id_base_) {
    return nullptr;  // a node from a previous stop (ids are never reused)
  }
  const std::size_t index = id - id_base_;
  if (index >= nodes_.size()) {
    return nullptr;
  }
  const Node& node = nodes_[index];
  return node.live ? &node : nullptr;  // tombstone → gone, same as a map miss
}

std::string DebugValueTree::PathKey(const Node& node) const {
  // Root→node chain joined by a unit separator (a byte that cannot appear in a
  // DAP variable name). Each segment is the sibling ordinal plus the name, so
  // two siblings that share a name — common in array pages and maps with
  // repeated labels — get distinct keys and expand/collapse independently.
  struct Segment {
    std::uint32_t ordinal;
    std::string_view name;
  };
  std::vector<Segment> parts;
  const Node* cur = &node;
  while (cur != nullptr) {
    // The sibling position is cached on the node at insertion (index in roots_ or
    // the parent's children vector), so this walk stays O(depth) rather than
    // rescanning the whole sibling vector per level.
    parts.push_back(Segment{cur->sibling_ordinal, cur->name});
    cur = cur->parent_id == 0 ? nullptr : FindNode(cur->parent_id);
  }
  std::string key;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (!key.empty()) {
      key.push_back('\x1f');
    }
    key.append(std::to_string(it->ordinal));
    key.push_back(':');
    key.append(it->name);
  }
  return key;
}

DebugValueTree::Node* DebugValueTree::FindNodeByReference(int variables_reference) {
  if (variables_reference <= 0) {
    return nullptr;
  }
  const auto it = reference_to_node_.find(variables_reference);
  return it == reference_to_node_.end() ? nullptr : FindNode(it->second);
}

std::uint32_t DebugValueTree::AddNode(Node node) {
  const std::uint32_t id = next_id_++;
  node.id = id;
  node.live = true;
  if (node.variables_reference > 0) {
    // First-writer-wins: a conformant adapter keeps variablesReference unique while a
    // stop is live, but a non-conformant one could recycle a still-live reference. Last-
    // write-wins would then remap the reference to the recycled child, so a later page
    // fetch for the original container would graft onto the wrong node. Keep the first
    // mapping (matches the bounded-paging / clamped-totals hostile-adapter defenses).
    reference_to_node_.try_emplace(node.variables_reference, id);
  }
  // Append-only: the invariant `nodes_.size() == next_id_ - id_base_` is what
  // makes the id its own index, and only DropAllNodes may re-base it.
  nodes_.push_back(std::move(node));
  ++live_nodes_;
  return id;
}

std::uint32_t DebugValueTree::AddRoot(std::string name, std::string value, std::string type,
                                      int variables_reference, bool is_scope, int total_count,
                                      bool total_known) {
  Node node;
  node.name = std::move(name);
  node.value = std::move(value);
  node.type = std::move(type);
  node.parent_id = 0;
  node.variables_reference = variables_reference;
  node.is_scope = is_scope;
  node.total_count = total_count;
  node.total_known = total_known;
  // Ordinal = index this node will occupy in roots_ (stable across rebuilds since
  // roots are appended in adapter order), so PathKey reads it in O(1).
  node.sibling_ordinal = static_cast<std::uint32_t>(roots_.size());
  const std::uint32_t id = AddNode(std::move(node));
  roots_.push_back(id);
  return id;
}

void DebugValueTree::EraseSubtree(std::uint32_t node_id) {
  // Iterative post-order-free walk: a hostile adapter can return a deeply nested
  // one-child tree, so recursing here (once per depth level) could overflow the
  // C++ stack. An explicit worklist keeps teardown O(nodes) with O(1) stack.
  std::vector<std::uint32_t> stack;
  stack.push_back(node_id);
  while (!stack.empty()) {
    const std::uint32_t id = stack.back();
    stack.pop_back();
    Node* node = FindNode(id);
    if (node == nullptr) {
      continue;
    }
    for (const std::uint32_t child_id : node->children) {
      stack.push_back(child_id);
    }
    const int reference = node->variables_reference;
    // Drop the reference mapping only if it still points at this node — first-
    // writer-wins may have handed our old reference to a sibling we must not strand.
    if (reference > 0) {
      const auto ref_it = reference_to_node_.find(reference);
      if (ref_it != reference_to_node_.end() && ref_it->second == id) {
        reference_to_node_.erase(ref_it);
      }
    }
    // Tombstone in place: the slot index IS the id, so removing the element would
    // renumber every later node. Move-assigning a default Node releases the three
    // strings and the child vector, so a dead slot costs no heap — only its
    // sizeof(Node) of already-reserved array space, reclaimed by DropAllNodes.
    *node = Node{};
    --live_nodes_;
  }
}

bool DebugValueTree::RebindReference(Node& node, int new_reference) {
  if (new_reference == node.variables_reference) {
    return false;
  }
  // The structure reference changed (e.g. a scalar became a container): drop any
  // stale children so a later expand refetches the new contents. Erase the old
  // mapping only if it still points at THIS node -- a non-conformant adapter may
  // have recycled our old reference onto a sibling that AddNode's first-writer-
  // wins kept, and an unconditional erase would strand that sibling. Reinstall
  // the new mapping with try_emplace to match AddNode's first-writer-wins.
  if (node.variables_reference > 0) {
    const auto it = reference_to_node_.find(node.variables_reference);
    if (it != reference_to_node_.end() && it->second == node.id) {
      reference_to_node_.erase(it);
    }
  }
  node.variables_reference = new_reference;
  // Erase the stale children's subtrees so they are not orphaned in the node maps
  // (node itself stays valid — only its descendants are removed).
  for (const std::uint32_t child_id : node.children) {
    EraseSubtree(child_id);
  }
  node.children.clear();
  node.children_loaded = false;
  node.expanded = false;
  if (node.variables_reference > 0) {
    reference_to_node_.try_emplace(node.variables_reference, node.id);
  }
  return true;
}

}  // namespace microide::workspace
