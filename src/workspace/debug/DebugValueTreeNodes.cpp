#include "workspace/debug/DebugValueTree.h"

#include <charconv>
#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

void DebugValueTree::Clear() {
  DropAllNodes();
  roots_.clear();
  rows_.clear();
  truncated_ = false;
  selected_row_ = 0;
  editing_node_.reset();
  edit_buffer_.SetText({});
}

void DebugValueTree::ClearRoots() {
  DropAllNodes();
  roots_.clear();
  truncated_ = false;
  editing_node_.reset();
}

void DebugValueTree::DropAllNodes() {
  nodes_.clear();
  reference_to_node_.clear();
  live_nodes_ = 0;
  id_base_ = next_id_;
}

DebugValueTree::Node* DebugValueTree::FindNode(std::uint32_t id) {
  return const_cast<Node*>(std::as_const(*this).FindNode(id));
}

const DebugValueTree::Node* DebugValueTree::FindNode(std::uint32_t id) const {
  if (id < id_base_) {
    return nullptr;
  }
  const std::size_t index = id - id_base_;
  if (index >= nodes_.size()) {
    return nullptr;
  }
  const Node& node = nodes_[index];
  return node.live ? &node : nullptr;
}

void DebugValueTree::PathKeyInto(const Node& node, std::string& out) const {
  path_key_segments_.clear();
  const Node* cur = &node;
  while (cur != nullptr) {
    path_key_segments_.push_back(PathKeySegment{cur->sibling_ordinal, cur->name});
    cur = cur->parent_id == 0 ? nullptr : FindNode(cur->parent_id);
  }
  out.clear();
  char ordinal_digits[16];
  for (auto it = path_key_segments_.rbegin(); it != path_key_segments_.rend(); ++it) {
    if (!out.empty()) {
      out.push_back('\x1f');
    }
    const auto [end, ec] =
        std::to_chars(ordinal_digits, ordinal_digits + sizeof(ordinal_digits), it->ordinal);
    if (ec == std::errc{}) {
      out.append(ordinal_digits, static_cast<std::size_t>(end - ordinal_digits));
    }
    out.push_back(':');
    out.append(it->name);
  }
}

std::string DebugValueTree::PathKey(const Node& node) const {
  std::string key;
  PathKeyInto(node, key);
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
    reference_to_node_.try_emplace(node.variables_reference, id);
  }
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
  node.sibling_ordinal = static_cast<std::uint32_t>(roots_.size());
  const std::uint32_t id = AddNode(std::move(node));
  roots_.push_back(id);
  return id;
}

void DebugValueTree::EraseSubtree(std::uint32_t node_id) {
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
    if (reference > 0) {
      const auto ref_it = reference_to_node_.find(reference);
      if (ref_it != reference_to_node_.end() && ref_it->second == id) {
        reference_to_node_.erase(ref_it);
      }
    }
    *node = Node{};
    --live_nodes_;
  }
}

bool DebugValueTree::RebindReference(Node& node, int new_reference) {
  if (new_reference == node.variables_reference) {
    return false;
  }
  if (node.variables_reference > 0) {
    const auto it = reference_to_node_.find(node.variables_reference);
    if (it != reference_to_node_.end() && it->second == node.id) {
      reference_to_node_.erase(it);
    }
  }
  node.variables_reference = new_reference;
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

}
