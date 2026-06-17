#include "workspace/DebugValueTree.h"

#include <algorithm>
#include <utility>

namespace microide::workspace {

void DebugValueTree::Clear() {
  nodes_.clear();
  reference_to_node_.clear();
  roots_.clear();
  rows_.clear();
  next_id_ = 1;
  selected_row_ = 0;
  editing_node_.reset();
  edit_buffer_.SetText({});
}

void DebugValueTree::ClearRoots() {
  nodes_.clear();
  reference_to_node_.clear();
  roots_.clear();
  next_id_ = 1;
  editing_node_.reset();
}

DebugValueTree::Node* DebugValueTree::FindNode(std::uint32_t id) {
  const auto it = nodes_.find(id);
  return it == nodes_.end() ? nullptr : &it->second;
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
  if (node.variables_reference > 0) {
    reference_to_node_[node.variables_reference] = id;
  }
  nodes_.emplace(id, std::move(node));
  return id;
}

std::uint32_t DebugValueTree::AddRoot(std::string name, std::string value, std::string type,
                                      int variables_reference, bool is_scope) {
  Node node;
  node.name = std::move(name);
  node.value = std::move(value);
  node.type = std::move(type);
  node.variables_reference = variables_reference;
  node.is_scope = is_scope;
  const std::uint32_t id = AddNode(std::move(node));
  roots_.push_back(id);
  return id;
}

int DebugValueTree::ToggleRow(std::size_t row_index) {
  if (row_index >= rows_.size()) {
    return 0;
  }
  Node* node = FindNode(rows_[row_index].node_id);
  if (node == nullptr || node->variables_reference <= 0) {
    return 0;  // leaf: nothing to expand
  }
  if (node->expanded) {
    node->expanded = false;
    Rebuild();
    return 0;
  }
  node->expanded = true;
  if (node->children_loaded) {
    Rebuild();
    return 0;
  }
  // Children not yet fetched: the caller issues a `variables` request and feeds
  // the result back through ApplyVariables. Rebuild now so the row shows expanded.
  const int reference = node->variables_reference;
  Rebuild();
  return reference;
}

void DebugValueTree::ApplyVariables(int variables_reference,
                                    const std::vector<dap_protocol::DapVariable>& variables) {
  Node* parent = FindNodeByReference(variables_reference);
  if (parent == nullptr) {
    return;
  }
  parent->children_loaded = true;
  parent->children.clear();
  const std::uint32_t parent_id = parent->id;
  for (const dap_protocol::DapVariable& variable : variables) {
    Node node;
    node.name = variable.name;
    node.value = variable.value;
    node.type = variable.type;
    node.variables_reference = variable.variables_reference;
    node.container_reference = variables_reference;
    const std::uint32_t child_id = AddNode(std::move(node));
    // Re-resolve parent: AddNode may have rehashed the node map.
    if (Node* reparent = FindNode(parent_id); reparent != nullptr) {
      reparent->children.push_back(child_id);
    }
  }
  Rebuild();
}

bool DebugValueTree::RebindReference(Node& node, int new_reference) {
  if (new_reference == node.variables_reference) {
    return false;
  }
  // The structure reference changed (e.g. a scalar became a container): drop any
  // stale children so a later expand refetches the new contents.
  if (node.variables_reference > 0) {
    reference_to_node_.erase(node.variables_reference);
  }
  node.variables_reference = new_reference;
  node.children.clear();
  node.children_loaded = false;
  node.expanded = false;
  if (node.variables_reference > 0) {
    reference_to_node_[node.variables_reference] = node.id;
  }
  return true;
}

void DebugValueTree::ApplySetVariable(std::uint32_t node_id,
                                      const dap_protocol::DapSetVariableResult& result) {
  Node* node = FindNode(node_id);
  if (node == nullptr) {
    return;
  }
  node->value = result.value;
  if (!result.type.empty()) {
    node->type = result.type;
  }
  RebindReference(*node, result.variables_reference);
  Rebuild();
}

void DebugValueTree::SetNodeValue(std::uint32_t node_id, std::string value, std::string type,
                                  int variables_reference) {
  Node* node = FindNode(node_id);
  if (node == nullptr) {
    return;
  }
  node->value = std::move(value);
  node->type = std::move(type);
  RebindReference(*node, variables_reference);
  Rebuild();
}

void DebugValueTree::Rebuild() {
  rows_.clear();
  for (const std::uint32_t root : roots_) {
    FlattenInto(root, 0);
  }
  if (selected_row_ >= rows_.size()) {
    selected_row_ = rows_.empty() ? 0 : rows_.size() - 1;
  }
}

void DebugValueTree::FlattenInto(std::uint32_t node_id, int depth) {
  const Node* node = FindNode(node_id);
  if (node == nullptr) {
    return;
  }
  DebugVariableRowView row;
  row.display_name = node->name;
  row.display_value = node->value;
  row.display_type = node->type;
  row.depth = depth;
  row.has_children = node->variables_reference > 0;
  row.expanded = node->expanded;
  row.editable = !node->is_scope;
  row.node_id = node->id;
  rows_.push_back(std::move(row));
  if (node->expanded) {
    for (const std::uint32_t child : node->children) {
      FlattenInto(child, depth + 1);
    }
  }
}

void DebugValueTree::SetSelectedRow(std::size_t row) {
  if (rows_.empty()) {
    selected_row_ = 0;
    return;
  }
  selected_row_ = std::min(row, rows_.size() - 1);
}

void DebugValueTree::MoveSelection(int delta) {
  if (rows_.empty()) {
    selected_row_ = 0;
    return;
  }
  const int max_index = static_cast<int>(rows_.size()) - 1;
  const int next = std::clamp(static_cast<int>(selected_row_) + delta, 0, max_index);
  selected_row_ = static_cast<std::size_t>(next);
}

bool DebugValueTree::BeginEdit(std::size_t row_index) {
  if (row_index >= rows_.size()) {
    return false;
  }
  Node* node = FindNode(rows_[row_index].node_id);
  if (node == nullptr || node->is_scope) {
    return false;
  }
  editing_node_ = node->id;
  selected_row_ = row_index;
  edit_buffer_.SetText(node->value);
  edit_buffer_.SelectAll();
  return true;
}

void DebugValueTree::CancelEdit() {
  editing_node_.reset();
  edit_buffer_.SetText({});
}

std::optional<DebugValueTree::EditTarget> DebugValueTree::EditTargetForCommit() const {
  if (!editing_node_.has_value()) {
    return std::nullopt;
  }
  const auto it = nodes_.find(*editing_node_);
  if (it == nodes_.end() || it->second.is_scope || it->second.container_reference <= 0) {
    return std::nullopt;
  }
  return EditTarget{
      .node_id = it->second.id,
      .container_reference = it->second.container_reference,
      .name = it->second.name,
  };
}

}  // namespace microide::workspace
