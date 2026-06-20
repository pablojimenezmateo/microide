#include "workspace/DebugValueTree.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "util/DebugTrace.h"

namespace microide::workspace {

namespace {

// Label shown on the synthetic placeholder row while a node's children are being
// fetched. Built in the model so the render TU only draws the prebuilt string.
constexpr std::string_view kPendingChildrenLabel = "loading…";
// Shown when a child fetch failed (adapter error / timeout): a finite end-state
// instead of a spinner that never resolves.
constexpr std::string_view kChildrenErrorLabel = "<unavailable>";
// Clickable affordance to fetch the next bounded page of a large container.
constexpr std::string_view kShowMoreLabel = "show more…";

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
      continue;  // float / exponent punctuation
    }
    return false;
  }
  return any_digit;
}

// Classify the value for coloring. Leaf-shaped kinds (string/bool/pointer/number)
// win over Aggregate so a pointer-with-children still reads as a pointer.
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

// Bound a page request to the children that actually exist. gdb's DAP throws
// "list index out of range" when count > available, so whenever the adapter told
// us the real total we clamp to it (clamping to a known 0 yields a count=0
// "fetch all" that simply returns the empty set). Only a genuinely unknown total
// (e.g. a watch root with no reported count) keeps the full page size as a
// bounded fallback that still guards against a garbage container.
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

}  // namespace

void DebugValueTree::Clear() {
  nodes_.clear();
  reference_to_node_.clear();
  roots_.clear();
  rows_.clear();
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
  nodes_.clear();
  reference_to_node_.clear();
  roots_.clear();
  // next_id_ stays monotonic across rebuilds — see Clear().
  editing_node_.reset();
}

DebugValueTree::Node* DebugValueTree::FindNode(std::uint32_t id) {
  const auto it = nodes_.find(id);
  return it == nodes_.end() ? nullptr : &it->second;
}

const DebugValueTree::Node* DebugValueTree::FindNode(std::uint32_t id) const {
  const auto it = nodes_.find(id);
  return it == nodes_.end() ? nullptr : &it->second;
}

std::string DebugValueTree::PathKey(const Node& node) const {
  // Root→node name chain joined by a unit separator (a byte that cannot appear in
  // a DAP variable name), so distinct paths never collide.
  std::vector<std::string_view> parts;
  const Node* cur = &node;
  while (cur != nullptr) {
    parts.emplace_back(cur->name);
    cur = cur->parent_id == 0 ? nullptr : FindNode(cur->parent_id);
  }
  std::string key;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (!key.empty()) {
      key.push_back('\x1f');
    }
    key.append(*it);
  }
  return key;
}

std::vector<DebugValueTree::ChildFetch> DebugValueTree::CollectAutoExpand(
    const std::vector<std::uint32_t>& node_ids) {
  std::vector<ChildFetch> fetches;
  for (const std::uint32_t id : node_ids) {
    Node* node = FindNode(id);
    if (node == nullptr || node->variables_reference <= 0 || node->expanded ||
        node->children_loaded || node->fetching) {
      continue;
    }
    if (!expanded_paths_.contains(PathKey(*node))) {
      continue;
    }
    node->expanded = true;
    // Known-empty container: nothing to fetch, just show it expanded-empty.
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
  // Once per session, seed the default scope (Locals) so it auto-expands through the
  // same bounded-fetch path a manual expand uses. Consuming the flag here means a
  // later collapse (which prunes the path key) is honored for the rest of the session.
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
  const std::uint32_t id = AddNode(std::move(node));
  roots_.push_back(id);
  return id;
}

DebugValueTree::ChildFetch DebugValueTree::ToggleRow(std::size_t row_index) {
  if (row_index >= rows_.size()) {
    return {};
  }
  // Synthetic loading/error rows are inert.
  if (rows_[row_index].is_placeholder) {
    return {};
  }
  // "Show more…" row: fetch the next bounded page of its parent's children. The
  // fetching guard prevents a double-click from issuing two overlapping pages.
  if (rows_[row_index].is_show_more) {
    Node* parent = FindNode(rows_[row_index].node_id);
    if (parent == nullptr || parent->variables_reference <= 0 || parent->fetching) {
      return {};
    }
    parent->fetching = true;
    const ChildFetch fetch{parent->variables_reference, parent->loaded_count,
                           BoundedPageCount(parent->total_known, parent->total_count,
                                            parent->loaded_count)};
    Rebuild();  // replaces the "show more…" row with a loading row
    return fetch;
  }
  Node* node = FindNode(rows_[row_index].node_id);
  if (node == nullptr || node->variables_reference <= 0) {
    return {};  // leaf: nothing to expand
  }
  if (node->expanded) {
    node->expanded = false;
    // Remember the collapse so a later stop does not re-expand it. Drop the node's
    // own key and any descendant keys (prefixed by it) so re-expanding the parent
    // later does not silently re-open everything beneath.
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
    // Already have children, or a fetch is already in flight: just show expanded
    // (the loading placeholder remains until the in-flight page lands).
    Rebuild();
    return {};
  }
  // A known-empty container (the adapter reported zero children, e.g. an empty
  // std::vector) has nothing to fetch: mark it loaded so it reads as expanded-empty
  // instead of issuing a count request gdb would reject.
  if (node->total_known && node->total_count == 0) {
    node->children_loaded = true;
    Rebuild();
    return {};
  }
  // Children not yet fetched: the caller issues a bounded `variables` request and
  // feeds the result back through ApplyVariables. Rebuild now so the row shows
  // expanded with a loading placeholder.
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
    parent->children.clear();
    parent->loaded_count = 0;
  }
  const std::uint32_t parent_id = parent->id;
  const int parent_total = parent->total_count;
  std::vector<std::uint32_t> new_child_ids;
  new_child_ids.reserve(variables.size());
  for (const dap_protocol::DapVariable& variable : variables) {
    Node node;
    node.name = variable.name;
    node.value = variable.value;
    node.type = variable.type;
    node.parent_id = parent_id;
    node.variables_reference = variable.variables_reference;
    node.container_reference = variables_reference;
    // The adapter's reported child-count (paging stops precisely; a reported 0
    // means an empty container). Only trusted when actually reported — otherwise
    // the fetch falls back to a bounded page.
    node.total_count = variable.indexed_variables + variable.named_variables;
    node.total_known = variable.count_reported;
    const std::uint32_t child_id = AddNode(std::move(node));
    new_child_ids.push_back(child_id);
    // `parent` stays valid across AddNode: unordered_map rehash on insert
    // invalidates iterators but never pointers/references to existing elements,
    // so we never need to re-resolve it per child.
    parent->children.push_back(child_id);
  }
  parent->loaded_count = static_cast<int>(parent->children.size());
  // More children remain? Use the adapter-reported total when known; otherwise
  // fall back to "a full page came back, so there may be more".
  if (parent->total_known) {
    parent->more_available = parent->loaded_count < parent_total;
  } else {
    parent->more_available = static_cast<int>(variables.size()) >= kChildPageSize;
  }
  // Restore any of these children the user had expanded before this stop.
  std::vector<ChildFetch> fetches = CollectAutoExpand(new_child_ids);
  Rebuild();
  return fetches;
}

void DebugValueTree::MarkChildrenError(int variables_reference) {
  // This is what turns a Locals/child row into "<unavailable>".
  util::DebugTrace::Note("locals", "mark-children-error ref",
                         static_cast<long long>(variables_reference));
  Node* parent = FindNodeByReference(variables_reference);
  if (parent == nullptr) {
    return;
  }
  parent->fetching = false;
  parent->more_available = false;
  // First-page failure: surface an error row. A failed "load more" keeps the
  // already-loaded children and simply stops offering more.
  if (parent->loaded_count == 0) {
    parent->children_loaded = true;
    parent->load_error = true;
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
  // Expanded but the first page is not here yet: show a single dim placeholder so
  // the expand reads as instant instead of frozen. ApplyVariables flips
  // children_loaded and rebuilds, replacing it with the real children.
  if (has_children && !node->children_loaded) {
    DebugVariableRowView placeholder;
    placeholder.display_name = std::string(kPendingChildrenLabel);
    placeholder.kind = DebugValueKind::Pending;
    placeholder.depth = depth + 1;
    placeholder.is_placeholder = true;
    rows_.push_back(std::move(placeholder));
    return;
  }
  // First-page fetch failed (adapter error / timeout): a finite error row instead
  // of a spinner that never resolves.
  if (node->load_error && node->children.empty()) {
    DebugVariableRowView error_row;
    error_row.display_name = std::string(kChildrenErrorLabel);
    error_row.kind = DebugValueKind::Error;
    error_row.depth = depth + 1;
    error_row.is_placeholder = true;
    rows_.push_back(std::move(error_row));
    return;
  }
  for (const std::uint32_t child : node->children) {
    FlattenInto(child, depth + 1);
  }
  // More children remain beyond the loaded page: a loading row while the next page
  // is in flight, otherwise a clickable "show more…" affordance.
  if (node->more_available) {
    DebugVariableRowView more_row;
    more_row.depth = depth + 1;
    more_row.node_id = node->id;  // so ToggleRow can map the click back to this node
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
