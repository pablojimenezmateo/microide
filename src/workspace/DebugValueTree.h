#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/DapProtocol.h"

namespace microide::workspace {

// Coarse classification of a value's display, computed in the model (off the
// render hot path) so the render TU maps kind → color with no string inspection.
// Drives both value coloring and the synthetic "loading…" placeholder row.
enum class DebugValueKind {
  Plain,      // fallback (unparsed scalar)
  Scope,      // a scope root: Locals / Registers (no value)
  Aggregate,  // struct / array / container (expandable, non-pointer)
  Number,     // integer or floating-point literal
  String,     // quoted string / char
  Pointer,    // address ("0x…") or pointer type
  Boolean,    // true / false
  Pending,    // synthetic placeholder shown while children load
  Error,      // synthetic placeholder shown when a child fetch failed
};

// One flattened, render-ready row of a debug value tree. Display strings are
// prebuilt (off the render hot path) so the bottom-panel render TU only draws
// them. `node_id` is the stable identity used to match the in-edit row and to
// route expand/edit actions; it survives flat-list rebuilds caused by an
// unrelated sibling expanding.
struct DebugVariableRowView {
  std::string display_name;   // e.g. "x" or "Locals" or a watch expression
  std::string display_value;  // e.g. "42" (empty for scopes)
  std::string display_type;   // e.g. "int" (muted, optional)
  DebugValueKind kind = DebugValueKind::Plain;
  int depth = 0;              // 0 = root (scope / watch expr), 1+ = nested
  bool has_children = false;  // structured (variablesReference > 0) → expandable
  bool expanded = false;
  bool editable = false;  // a real variable (not a scope) → can enter setVariable edit
  bool is_placeholder = false;  // synthetic "loading…"/error row: not selectable/clickable
  bool is_show_more = false;    // synthetic "show more…" row: click loads the next page
  std::uint32_t node_id = 0;
};

// The reusable lazy value tree shared by the Variables and Watch panels. Source
// of truth is a tree of nodes keyed by a stable monotonic id; a prebuilt flat
// row list is rematerialized whenever the tree changes so render/click are O(1)
// by row index. The tree itself performs no I/O: the owning model / DebugService
// drives `variables`/`setVariable` requests and feeds responses back through the
// Apply* methods.
//
// Roots are installed by the owner (scope nodes for Variables, watch-expression
// result nodes for Watch) via AddRoot + Rebuild; everything below — lazy expand,
// child attach, inline edit, selection — is generic and shared.
class DebugValueTree {
 public:
  // The variable currently being edited inline: the container reference + name
  // that setVariable needs, plus the node id to apply the result back onto.
  struct EditTarget {
    std::uint32_t node_id = 0;
    int container_reference = 0;
    std::string name;
  };

  // A bounded child fetch the owner must issue on the adapter: `count` children of
  // `reference` starting at `start`. `reference == 0` means "nothing to fetch"
  // (collapse, leaf, already-loaded, or a fetch already in flight).
  struct ChildFetch {
    int reference = 0;
    int start = 0;
    int count = 0;
  };

  // Page size for lazy child fetches. Bounding the request is what stops an
  // adapter from enumerating a container in one shot — critical because a garbage
  // / uninitialized container can report billions of elements, which would make
  // gdb allocate without bound and freeze the host.
  static constexpr int kChildPageSize = 200;

  // Name of the root scope auto-expanded once per session (open by default). Empty
  // (the default) disables it; the Variables model sets "Locals". The shared tree is
  // otherwise scope-agnostic, so the Watch panel never auto-expands.
  void SetDefaultExpandedScope(std::string name) { default_expanded_scope_ = std::move(name); }

  // Start a fresh debug session: forget remembered expansion and re-arm the one-shot
  // default expansion so the default scope (Locals) opens on the session's first stop.
  // Unlike Clear() (per stop), this is called only when a new session launches.
  void ResetExpansionForNewSession() {
    expanded_paths_.clear();
    pending_default_expansion_ = true;
  }

  // Clear everything (nodes, rows, selection, edit state).
  void Clear();

  // Drop all roots/nodes/edit state in preparation for installing a fresh set of
  // roots; keeps nothing but the selection cursor (clamped on the next Rebuild).
  void ClearRoots();

  // Append a root node and return its id. Call Rebuild() once all roots/children
  // for a refresh have been added. `is_scope` roots are not editable and render
  // without a value. `total_count` is the adapter-reported child count (0 when
  // unknown); it bounds the lazy fetch so we never request more children than
  // exist (gdb's DAP errors on count > available).
  std::uint32_t AddRoot(std::string name, std::string value, std::string type,
                        int variables_reference, bool is_scope, int total_count = 0,
                        bool total_known = false);

  // Re-expand the roots (and cascade) that the user had open before the tree was
  // rebuilt for a new stop. Variables references are not stable across stops, so
  // expansion is tracked by node path (name chain). Returns the bounded fetches
  // the owner must issue to repopulate each re-expanded container; the children
  // that arrive (ApplyVariables) cascade further. Call after installing roots,
  // before Rebuild().
  std::vector<ChildFetch> RestoreExpandedRoots();

  // Rematerialize the flat row list from the current tree.
  void Rebuild();

  // Toggle expansion of the row at `row_index`, or load the next page when the row
  // is a synthetic "show more…" affordance. Returns the bounded ChildFetch the
  // owner must issue (reference > 0) when a page must be fetched; otherwise an
  // empty ChildFetch (collapse, leaf, already-loaded, in-flight, or a synthetic
  // loading/error row — the flat list is rebuilt in place).
  ChildFetch ToggleRow(std::size_t row_index);

  // Attach the children fetched for `variables_reference` to its owning node.
  // `start` is the page offset that was requested: 0 replaces the child list, a
  // positive offset appends (paging). Clears the loading/error state for the node.
  // Returns the bounded fetches for any newly-attached children that were
  // previously expanded (path-tracked) and must be repopulated to restore the
  // user's open tree across a stop; empty in the common (no restore) case.
  std::vector<ChildFetch> ApplyVariables(int variables_reference,
                                         const std::vector<dap_protocol::DapVariable>& variables,
                                         int start);

  // Mark a node's child fetch as failed (adapter error / timeout): clears the
  // loading state so the row shows an error instead of a permanent spinner. A
  // first-page failure surfaces an "<unavailable>" row; a failed "load more"
  // keeps the loaded children and stops offering more. No-op for an unknown ref.
  void MarkChildrenError(int variables_reference);

  // Reflect a setVariable response onto the edited node (value/type, and a
  // possibly-new structure reference).
  void ApplySetVariable(std::uint32_t node_id, const dap_protocol::DapSetVariableResult& result);

  // Fold an async `evaluate` result onto an existing node (e.g. a pre-created
  // watch-expression root): update value/type and re-bind the child container
  // reference. No-op for an unknown id. Rebuilds the flat list.
  void SetNodeValue(std::uint32_t node_id, std::string value, std::string type,
                    int variables_reference);

  const std::vector<DebugVariableRowView>& Rows() const { return rows_; }
  bool Empty() const { return rows_.empty(); }

  // Keyboard selection cursor (mouse click sets it directly too).
  std::size_t SelectedRow() const { return selected_row_; }
  void SetSelectedRow(std::size_t row);
  void MoveSelection(int delta);

  // Inline edit lifecycle. BeginEdit seeds the buffer with the node's current
  // value and selects all; it fails for scopes or out-of-range rows.
  bool BeginEdit(std::size_t row_index);
  void CancelEdit();
  bool IsEditing() const { return editing_node_.has_value(); }
  std::optional<std::uint32_t> EditingNodeId() const { return editing_node_; }
  std::optional<EditTarget> EditTargetForCommit() const;
  editor::SingleLineEditor& EditBuffer() { return edit_buffer_; }
  const editor::SingleLineEditor& EditBuffer() const { return edit_buffer_; }

 private:
  struct Node {
    std::uint32_t id = 0;
    std::string name;
    std::string value;
    std::string type;
    std::uint32_t parent_id = 0;       // owning node id (0 = root); used to build the path key
    std::uint32_t sibling_ordinal = 0;  // this node's index among its siblings (roots_ or parent->children); part of the stable path key
    int variables_reference = 0;       // this node's own container ref (children)
    int container_reference = 0;       // the ref of the container holding this node (for setVariable)
    bool is_scope = false;
    bool expanded = false;
    bool children_loaded = false;  // the first page has arrived
    bool fetching = false;         // a child-page request is in flight (in-flight guard)
    bool more_available = false;   // more children remain beyond what is loaded
    bool load_error = false;       // the last child fetch failed
    int loaded_count = 0;          // children fetched so far
    int total_count = 0;           // total children when total_known (0 = empty)
    bool total_known = false;      // the adapter reported this node's child count
    std::vector<std::uint32_t> children;
  };

  Node* FindNode(std::uint32_t id);
  const Node* FindNode(std::uint32_t id) const;
  Node* FindNodeByReference(int variables_reference);
  std::uint32_t AddNode(Node node);
  // Recursively remove a node and all its descendants from nodes_ and
  // reference_to_node_. Used when a container's child list is replaced (a fresh
  // start<=0 page or a RebindReference) so the old child Node objects and their
  // reference mappings are not orphaned/leaked in the maps.
  void EraseSubtree(std::uint32_t node_id);
  // Path key (root→node name chain) used to track expansion across rebuilds, since
  // variables references are not stable between stops.
  std::string PathKey(const Node& node) const;
  // For each id, if the node is an unfetched container the user had expanded
  // (its path is in expanded_paths_), mark it expanded + fetching and emit its
  // bounded ChildFetch. Empty/known-empty containers are marked loaded, no fetch.
  std::vector<ChildFetch> CollectAutoExpand(const std::vector<std::uint32_t>& node_ids);
  // Re-point a node's child container reference, keeping reference_to_node_ and
  // the (now stale) loaded children consistent. Returns true when it changed.
  bool RebindReference(Node& node, int new_reference);
  void FlattenInto(std::uint32_t node_id, int depth);

  std::unordered_map<std::uint32_t, Node> nodes_;
  std::unordered_map<int, std::uint32_t> reference_to_node_;  // variables_reference → node id
  std::vector<std::uint32_t> roots_;                          // root node ids, in order
  // Path keys (root→node name chains) of containers the user has expanded. Tracked
  // across ClearRoots/Clear so expansion survives a stop; pruned on collapse.
  std::unordered_set<std::string> expanded_paths_;
  // The root scope auto-expanded once at the start of each session (empty = none).
  // The Variables model sets this to "Locals" (open by default); the shared tree
  // itself is scope-agnostic so the Watch panel is unaffected. Seeded into
  // expanded_paths_ on the first ApplyScopes after a Clear(), then left to the
  // normal expanded_paths_ machinery — an explicit collapse within the session is
  // respected, and a new session reopens it.
  std::string default_expanded_scope_;
  bool pending_default_expansion_ = true;
  std::vector<DebugVariableRowView> rows_;
  std::uint32_t next_id_ = 1;
  std::size_t selected_row_ = 0;
  std::optional<std::uint32_t> editing_node_;
  editor::SingleLineEditor edit_buffer_;
};

}  // namespace microide::workspace
