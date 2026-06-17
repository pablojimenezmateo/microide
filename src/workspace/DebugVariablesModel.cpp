#include "workspace/DebugVariablesModel.h"

#include <algorithm>
#include <utility>

namespace microide::workspace {

void DebugVariablesModel::Clear() {
  nodes_.clear();
  reference_to_node_.clear();
  roots_.clear();
  rows_.clear();
  next_id_ = 1;
  frame_id_ = 0;
  selected_row_ = 0;
  editing_node_.reset();
  edit_buffer_.SetText({});
}

void DebugVariablesModel::BeginFrame(int frame_id) {
  Clear();
  frame_id_ = frame_id;
}

DebugVariablesModel::Node* DebugVariablesModel::FindNode(std::uint32_t id) {
  const auto it = nodes_.find(id);
  return it == nodes_.end() ? nullptr : &it->second;
}

DebugVariablesModel::Node* DebugVariablesModel::FindNodeByReference(int variables_reference) {
  if (variables_reference <= 0) {
    return nullptr;
  }
  const auto it = reference_to_node_.find(variables_reference);
  return it == reference_to_node_.end() ? nullptr : FindNode(it->second);
}

std::uint32_t DebugVariablesModel::AddNode(Node node) {
  const std::uint32_t id = next_id_++;
  node.id = id;
  if (node.variables_reference > 0) {
    reference_to_node_[node.variables_reference] = id;
  }
  nodes_.emplace(id, std::move(node));
  return id;
}

void DebugVariablesModel::ApplyScopes(const std::vector<dap_protocol::DapScope>& scopes) {
  nodes_.clear();
  reference_to_node_.clear();
  roots_.clear();
  next_id_ = 1;
  editing_node_.reset();
  for (const dap_protocol::DapScope& scope : scopes) {
    Node node;
    node.name = scope.name;
    node.variables_reference = scope.variables_reference;
    node.is_scope = true;
    roots_.push_back(AddNode(std::move(node)));
  }
  Flatten();
}

int DebugVariablesModel::ToggleRow(std::size_t row_index) {
  if (row_index >= rows_.size()) {
    return 0;
  }
  Node* node = FindNode(rows_[row_index].node_id);
  if (node == nullptr || node->variables_reference <= 0) {
    return 0;  // leaf: nothing to expand
  }
  if (node->expanded) {
    node->expanded = false;
    Flatten();
    return 0;
  }
  node->expanded = true;
  if (node->children_loaded) {
    Flatten();
    return 0;
  }
  // Children not yet fetched: the caller issues a `variables` request and feeds
  // the result back through ApplyVariables. Flatten now so the row shows expanded.
  const int reference = node->variables_reference;
  Flatten();
  return reference;
}

void DebugVariablesModel::ApplyVariables(
    int variables_reference, const std::vector<dap_protocol::DapVariable>& variables) {
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
  Flatten();
}

void DebugVariablesModel::ApplySetVariable(std::uint32_t node_id,
                                           const dap_protocol::DapSetVariableResult& result) {
  Node* node = FindNode(node_id);
  if (node == nullptr) {
    return;
  }
  node->value = result.value;
  if (!result.type.empty()) {
    node->type = result.type;
  }
  // The structure reference can change (e.g. a scalar became a container): drop
  // any stale children so a later expand refetches the new contents.
  if (result.variables_reference != node->variables_reference) {
    if (node->variables_reference > 0) {
      reference_to_node_.erase(node->variables_reference);
    }
    node->variables_reference = result.variables_reference;
    node->children.clear();
    node->children_loaded = false;
    node->expanded = false;
    if (node->variables_reference > 0) {
      reference_to_node_[node->variables_reference] = node->id;
    }
  }
  Flatten();
}

void DebugVariablesModel::Flatten() {
  rows_.clear();
  for (const std::uint32_t root : roots_) {
    FlattenInto(root, 0);
  }
  if (selected_row_ >= rows_.size()) {
    selected_row_ = rows_.empty() ? 0 : rows_.size() - 1;
  }
}

void DebugVariablesModel::FlattenInto(std::uint32_t node_id, int depth) {
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

void DebugVariablesModel::SetSelectedRow(std::size_t row) {
  if (rows_.empty()) {
    selected_row_ = 0;
    return;
  }
  selected_row_ = std::min(row, rows_.size() - 1);
}

void DebugVariablesModel::MoveSelection(int delta) {
  if (rows_.empty()) {
    selected_row_ = 0;
    return;
  }
  const int max_index = static_cast<int>(rows_.size()) - 1;
  const int next = std::clamp(static_cast<int>(selected_row_) + delta, 0, max_index);
  selected_row_ = static_cast<std::size_t>(next);
}

bool DebugVariablesModel::BeginEdit(std::size_t row_index) {
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

void DebugVariablesModel::CancelEdit() {
  editing_node_.reset();
  edit_buffer_.SetText({});
}

std::optional<DebugVariablesModel::EditTarget> DebugVariablesModel::EditTargetForCommit() const {
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
