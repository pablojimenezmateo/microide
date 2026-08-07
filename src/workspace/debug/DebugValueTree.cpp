#include "workspace/debug/DebugValueTree.h"

#include <algorithm>
#include <limits>
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
// Terminal row appended when the aggregate loaded-node budget (kMaxLoadedNodes) has
// dropped children: signals the model is intentionally incomplete (TD-2026-07-17A-040).
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

std::vector<DebugValueTree::ChildFetch> DebugValueTree::CollectAutoExpand(
    const std::vector<std::uint32_t>& node_ids) {
  std::vector<ChildFetch> fetches;
  // With no remembered expansions there is nothing to restore, so skip the
  // per-node PathKey construction below entirely. PathKey walks a node's whole
  // ancestor chain (O(depth)); without this guard, incrementally loading a very
  // deep chain one page at a time costs O(depth^2) in wasted PathKey work even
  // though not a single child can match the empty set.
  if (expanded_paths_.empty()) {
    return fetches;
  }
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
    // A fresh first page replaces the child list; erase the previous children's
    // subtrees so their Node objects and reference mappings are not orphaned in
    // nodes_/reference_to_node_. `parent` stays valid (only OTHER nodes are erased).
    for (const std::uint32_t child_id : parent->children) {
      EraseSubtree(child_id);
    }
    parent->children.clear();
    parent->loaded_count = 0;
  }
  // Everything the attach loop needs from `parent` is read out here, because the
  // loop must not hold a Node* across AddNode: nodes_ is a vector and push_back
  // can reallocate it (TD-2026-08-07-162). The parent is re-resolved once after
  // the loop instead of chased per child.
  const std::uint32_t parent_id = parent->id;
  const int parent_total = parent->total_count;
  const std::size_t base_ordinal = parent->children.size();
  const bool parent_total_known = parent->total_known;
  parent = nullptr;
  std::vector<std::uint32_t> new_child_ids;
  new_child_ids.reserve(variables.size());
  bool hit_node_budget = false;
  for (const dap_protocol::DapVariable& variable : variables) {
    // TD-2026-07-17A-040: aggregate loaded-node budget. Repeated paging across many
    // containers can grow the tree past kMaxLoadedNodes even though each page is
    // bounded; stop attaching once the budget is hit and flag truncation.
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
    // The adapter's reported child-count (paging stops precisely; a reported 0
    // means an empty container). Only trusted when actually reported — otherwise
    // the fetch falls back to a bounded page.
    // Sum in 64-bit with negatives clamped: both fields are ints truncated from
    // the adapter's JSON int64, so an int+int here would overflow (UB) and a
    // negative would corrupt the paging math (total_count - loaded_count).
    const long long child_total =
        std::max<long long>(0, variable.indexed_variables) +
        std::max<long long>(0, variable.named_variables);
    node.total_count = static_cast<int>(
        std::min<long long>(child_total, std::numeric_limits<int>::max()));
    node.total_known = variable.count_reported;
    // Ordinal = index this child will occupy in parent->children (children are
    // appended in page order and refilled wholesale on reload), so PathKey reads
    // it in O(1) instead of rescanning the sibling vector per node.
    node.sibling_ordinal = static_cast<std::uint32_t>(base_ordinal + new_child_ids.size());
    new_child_ids.push_back(AddNode(std::move(node)));
  }
  // Re-resolve after the inserts. The parent cannot have been erased in between
  // (AddNode only appends), so a null here would be a broken invariant, not a
  // reachable state — but bail rather than dereference if it ever is.
  parent = FindNode(parent_id);
  if (parent == nullptr) {
    return {};
  }
  parent->children.insert(parent->children.end(), new_child_ids.begin(), new_child_ids.end());
  parent->loaded_count = static_cast<int>(parent->children.size());
  // More children remain? Use the adapter-reported total when known; otherwise
  // fall back to "a full page came back, so there may be more".
  if (parent_total_known) {
    parent->more_available = parent->loaded_count < parent_total;
  } else {
    parent->more_available = static_cast<int>(variables.size()) >= kChildPageSize;
  }
  // A page requested past the already-loaded children (start > 0) that returns
  // NOTHING means the adapter has no more to give, even if its reported total_count
  // over-counts. Force more_available false so the "show more…" affordance does not
  // persist forever for an over-reporting adapter that answers the page with an
  // empty success rather than an error (which MarkChildrenError would have cleared).
  if (start > 0 && variables.empty()) {
    parent->more_available = false;
  }
  // Budget-truncated page: stop offering more of this node so the "show more…"
  // affordance does not persist for children we deliberately refused to attach.
  if (hit_node_budget) {
    parent->more_available = false;
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
  // TD-2026-07-17A-040: a single terminal, non-selectable row makes the aggregate
  // node-budget truncation visible instead of silently dropping values.
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
  // Bound recursion depth: a hostile adapter can nest containers thousands deep,
  // and (with auto-expanded scopes) FlattenInto would recurse per level and could
  // overflow the C++ stack. Nothing beyond this depth is usefully visible, so stop
  // descending there — real debuggees never approach it. The guard sits at the
  // recursion site (not the function entry) so the common leaf call, which has no
  // children and never enters this loop, pays nothing for it.
  constexpr int kMaxFlattenDepth = 256;
  if (depth < kMaxFlattenDepth) {
    for (const std::uint32_t child : node->children) {
      FlattenInto(child, depth + 1);
    }
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
  // Synthetic placeholder / "show more…" rows carry their parent container's
  // node_id (see FlattenInto), so editing one would inline-edit the parent node.
  // Reject them here as ToggleRow does — defense-in-depth against a caller that
  // reaches BeginEdit without the editable/!has_children gate.
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

}  // namespace microide::workspace
