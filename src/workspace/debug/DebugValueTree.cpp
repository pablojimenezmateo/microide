#include "workspace/debug/DebugValueTree.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

#include "util/DebugTrace.h"

namespace microide::workspace {

namespace {

constexpr std::string_view kPendingChildrenLabel = "loading…";
constexpr std::string_view kChildrenErrorLabel = "<unavailable>";
constexpr std::string_view kShowMoreLabel = "show more…";
constexpr std::string_view kTruncatedLabel = "…(too many values — truncated)";

bool LooksNumeric(std::string_view value) {
  std::size_t i = 0;
  if (i < value.size() && (value[i] == '-' || value[i] == '+')) {
    ++i;
  }
  if (i >= value.size()) {
    return false;
  }
  bool any_digit = false;
  for (; i < value.size(); ++i) {
    const char c = value[i];
    if (c >= '0' && c <= '9') {
      any_digit = true;
      continue;
    }
    if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
      continue;
    }
    return false;
  }
  return any_digit;
}

DebugValueKind ClassifyValue(std::string_view value, std::string_view type, bool is_scope,
                             bool has_children) {
  if (is_scope) {
    return DebugValueKind::Scope;
  }
  if (!value.empty()) {
    const char front = value.front();
    if (front == '"' || front == '\'') {
      return DebugValueKind::String;
    }
    if (value == "true" || value == "false") {
      return DebugValueKind::Boolean;
    }
    if (value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
      return DebugValueKind::Pointer;
    }
  }
  if (!type.empty() && type.back() == '*') {
    return DebugValueKind::Pointer;
  }
  if (LooksNumeric(value)) {
    return DebugValueKind::Number;
  }
  if (has_children) {
    return DebugValueKind::Aggregate;
  }
  return DebugValueKind::Plain;
}

int BoundedPageCount(bool total_known, int total_count, int already_loaded) {
  if (!total_known) {
    return DebugValueTree::kChildPageSize;
  }
  const int remaining = total_count - already_loaded;
  if (remaining <= 0) {
    return 0;
  }
  return std::min(DebugValueTree::kChildPageSize, remaining);
}

}

std::vector<DebugValueTree::ChildFetch> DebugValueTree::CollectAutoExpand(
    const std::vector<std::uint32_t>& node_ids) {
  std::vector<ChildFetch> fetches;
  if (expanded_paths_.empty()) {
    return fetches;
  }
  for (const std::uint32_t id : node_ids) {
    Node* node = FindNode(id);
    if (node == nullptr || node->variables_reference <= 0 || node->expanded ||
        node->children_loaded || node->fetching) {
      continue;
    }
    PathKeyInto(*node, path_key_scratch_);
    if (!expanded_paths_.contains(path_key_scratch_)) {
      continue;
    }
    node->expanded = true;
    if (node->total_known && node->total_count == 0) {
      node->children_loaded = true;
      continue;
    }
    node->fetching = true;
    fetches.push_back(ChildFetch{node->variables_reference, 0,
                                 BoundedPageCount(node->total_known, node->total_count, 0)});
  }
  return fetches;
}

std::vector<DebugValueTree::ChildFetch> DebugValueTree::RestoreExpandedRoots() {
  if (pending_default_expansion_) {
    pending_default_expansion_ = false;
    if (!default_expanded_scope_.empty()) {
      for (const std::uint32_t id : roots_) {
        const Node* node = FindNode(id);
        if (node != nullptr && node->name == default_expanded_scope_) {
          expanded_paths_.insert(PathKey(*node));
        }
      }
    }
  }
  return CollectAutoExpand(roots_);
}

DebugValueTree::ChildFetch DebugValueTree::ToggleRow(std::size_t row_index) {
  if (row_index >= rows_.size()) {
    return {};
  }
  if (rows_[row_index].is_placeholder) {
    return {};
  }
  if (rows_[row_index].is_show_more) {
    Node* parent = FindNode(rows_[row_index].node_id);
    if (parent == nullptr || parent->variables_reference <= 0 || parent->fetching) {
      return {};
    }
    parent->fetching = true;
    const ChildFetch fetch{parent->variables_reference, parent->loaded_count,
                           BoundedPageCount(parent->total_known, parent->total_count,
                                            parent->loaded_count)};
    Rebuild();
    return fetch;
  }
  Node* node = FindNode(rows_[row_index].node_id);
  if (node == nullptr || node->variables_reference <= 0) {
    return {};
  }
  if (node->expanded) {
    node->expanded = false;
    const std::string key = PathKey(*node);
    const std::string descendant_prefix = key + '\x1f';
    for (auto it = expanded_paths_.begin(); it != expanded_paths_.end();) {
      if (*it == key || it->starts_with(descendant_prefix)) {
        it = expanded_paths_.erase(it);
      } else {
        ++it;
      }
    }
    Rebuild();
    return {};
  }
  node->expanded = true;
  expanded_paths_.insert(PathKey(*node));
  if (node->children_loaded || node->fetching) {
    Rebuild();
    return {};
  }
  if (node->total_known && node->total_count == 0) {
    node->children_loaded = true;
    Rebuild();
    return {};
  }
  node->fetching = true;
  const ChildFetch fetch{node->variables_reference, 0,
                         BoundedPageCount(node->total_known, node->total_count, 0)};
  Rebuild();
  return fetch;
}

std::vector<DebugValueTree::ChildFetch> DebugValueTree::ApplyVariables(
    int variables_reference, const std::vector<dap_protocol::DapVariable>& variables, int start) {
  util::DebugTrace::Note("locals", "apply-variables ref", static_cast<long long>(variables_reference),
                         static_cast<long long>(variables.size()));
  Node* parent = FindNodeByReference(variables_reference);
  if (parent == nullptr) {
    util::DebugTrace::Note("locals", "apply-variables: no parent for ref",
                           static_cast<long long>(variables_reference));
    return {};
  }
  parent->fetching = false;
  parent->children_loaded = true;
  parent->load_error = false;
  if (start <= 0) {
    for (const std::uint32_t child_id : parent->children) {
      EraseSubtree(child_id);
    }
    parent->children.clear();
    parent->loaded_count = 0;
  }
  const std::uint32_t parent_id = parent->id;
  const int parent_total = parent->total_count;
  const std::size_t base_ordinal = parent->children.size();
  const bool parent_total_known = parent->total_known;
  parent = nullptr;
  std::vector<std::uint32_t> new_child_ids;
  new_child_ids.reserve(variables.size());
  bool hit_node_budget = false;
  for (const dap_protocol::DapVariable& variable : variables) {
    if (live_nodes_ >= kMaxLoadedNodes || nodes_.size() >= kMaxNodeSlots) {
      truncated_ = true;
      hit_node_budget = true;
      break;
    }
    Node node;
    node.name = variable.name;
    node.value = variable.value;
    node.type = variable.type;
    node.parent_id = parent_id;
    node.variables_reference = variable.variables_reference;
    node.container_reference = variables_reference;
    const long long child_total =
        std::max<long long>(0, variable.indexed_variables) +
        std::max<long long>(0, variable.named_variables);
    node.total_count = static_cast<int>(
        std::min<long long>(child_total, std::numeric_limits<int>::max()));
    node.total_known = variable.count_reported;
    node.sibling_ordinal = static_cast<std::uint32_t>(base_ordinal + new_child_ids.size());
    new_child_ids.push_back(AddNode(std::move(node)));
  }
  parent = FindNode(parent_id);
  if (parent == nullptr) {
    return {};
  }
  parent->children.insert(parent->children.end(), new_child_ids.begin(), new_child_ids.end());
  parent->loaded_count = static_cast<int>(parent->children.size());
  if (parent_total_known) {
    parent->more_available = parent->loaded_count < parent_total;
  } else {
    parent->more_available = static_cast<int>(variables.size()) >= kChildPageSize;
  }
  if (start > 0 && variables.empty()) {
    parent->more_available = false;
  }
  if (hit_node_budget) {
    parent->more_available = false;
  }
  std::vector<ChildFetch> fetches = CollectAutoExpand(new_child_ids);
  Rebuild();
  return fetches;
}

void DebugValueTree::MarkChildrenError(int variables_reference) {
  util::DebugTrace::Note("locals", "mark-children-error ref",
                         static_cast<long long>(variables_reference));
  Node* parent = FindNodeByReference(variables_reference);
  if (parent == nullptr) {
    return;
  }
  parent->fetching = false;
  parent->more_available = false;
  if (parent->loaded_count == 0) {
    parent->children_loaded = true;
    parent->load_error = true;
  }
  Rebuild();
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
  if (truncated_) {
    DebugVariableRowView truncated_row;
    truncated_row.display_name = std::string(kTruncatedLabel);
    truncated_row.kind = DebugValueKind::Error;
    truncated_row.depth = 0;
    truncated_row.is_placeholder = true;
    rows_.push_back(std::move(truncated_row));
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
  const bool has_children = node->variables_reference > 0;
  DebugVariableRowView row;
  row.display_name = node->name;
  row.display_value = node->value;
  row.display_type = node->type;
  row.kind = ClassifyValue(node->value, node->type, node->is_scope, has_children);
  row.depth = depth;
  row.has_children = has_children;
  row.expanded = node->expanded;
  row.editable = !node->is_scope;
  row.node_id = node->id;
  rows_.push_back(std::move(row));
  if (!node->expanded) {
    return;
  }
  if (has_children && !node->children_loaded) {
    DebugVariableRowView placeholder;
    placeholder.display_name = std::string(kPendingChildrenLabel);
    placeholder.kind = DebugValueKind::Pending;
    placeholder.depth = depth + 1;
    placeholder.is_placeholder = true;
    rows_.push_back(std::move(placeholder));
    return;
  }
  if (node->load_error && node->children.empty()) {
    DebugVariableRowView error_row;
    error_row.display_name = std::string(kChildrenErrorLabel);
    error_row.kind = DebugValueKind::Error;
    error_row.depth = depth + 1;
    error_row.is_placeholder = true;
    rows_.push_back(std::move(error_row));
    return;
  }
  constexpr int kMaxFlattenDepth = 256;
  if (depth < kMaxFlattenDepth) {
    for (const std::uint32_t child : node->children) {
      FlattenInto(child, depth + 1);
    }
  }
  if (node->more_available) {
    DebugVariableRowView more_row;
    more_row.depth = depth + 1;
    more_row.node_id = node->id;
    if (node->fetching) {
      more_row.display_name = std::string(kPendingChildrenLabel);
      more_row.kind = DebugValueKind::Pending;
      more_row.is_placeholder = true;
    } else {
      more_row.display_name = std::string(kShowMoreLabel);
      more_row.kind = DebugValueKind::Pending;
      more_row.is_show_more = true;
    }
    rows_.push_back(std::move(more_row));
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
  if (rows_[row_index].is_placeholder || rows_[row_index].is_show_more) {
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
  const Node* node = FindNode(*editing_node_);
  if (node == nullptr || node->is_scope || node->container_reference <= 0) {
    return std::nullopt;
  }
  return EditTarget{
      .node_id = node->id,
      .container_reference = node->container_reference,
      .name = node->name,
  };
}

}
